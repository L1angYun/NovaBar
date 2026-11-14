using System.Net;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace NovaBar.Core;

public sealed record UpdatePackage
{
    public string Version { get; }
    public string FileName { get; }
    public Uri DownloadUri { get; }
    public string Sha256 { get; }
    public long Size { get; }

    internal UpdateSourcePreference Source { get; }
    internal UpdateSourcePreference Preference { get; }

    internal UpdatePackage(string version, string fileName, Uri downloadUri, string sha256, long size,
        UpdateSourcePreference source, UpdateSourcePreference preference) =>
        (Version, FileName, DownloadUri, Sha256, Size, Source, Preference) = (version, fileName, downloadUri, sha256, size, source, preference);
}

public sealed record UpdateCheckResult(string Version, bool IsNewer, string ReleaseName,
    string ReleaseNotes, Uri ReleaseUri, UpdatePackage? Installer)
{
    public UpdateSourcePreference Source { get; init; } = UpdateSourcePreference.GitHub;
}

/// <summary>Reads stable releases and integrity metadata from NovaBar's official GitHub and Gitee repositories.</summary>
public sealed class UpdateChecker(HttpClient client)
{
    public const string RepositoryRoot = "https://github.com/L1angYun/NovaBar";
    public static readonly Uri LatestReleaseUri = new("https://api.github.com/repos/L1angYun/NovaBar/releases/latest");
    public const long MaximumInstallerSize = 512L * 1024 * 1024;

    public static bool TryParseVersion(string? value, out Version version)
    {
        version = new Version(0, 0, 0);
        if (value is null || value.Length > 40 || !Regex.IsMatch(value, @"\Av?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\z", RegexOptions.CultureInvariant)) return false;
        if (!System.Version.TryParse(value.TrimStart('v'), out var parsed)) return false;
        version = parsed;
        return true;
    }

    public Task<UpdateCheckResult?> CheckAsync(string currentVersion, CancellationToken cancellationToken = default) =>
        CheckAsync(currentVersion, UpdateSourcePreference.Auto, cancellationToken);

    public async Task<UpdateCheckResult?> CheckAsync(string currentVersion, UpdateSourcePreference preference,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!Enum.IsDefined(preference)) throw new ArgumentOutOfRangeException(nameof(preference));
        if (!TryParseVersion(currentVersion, out var current)) throw new ArgumentException("Invalid current application version.", nameof(currentVersion));
        if (preference != UpdateSourcePreference.Auto)
        {
            var selected = await TrySourceAsync(preference, preference, current, cancellationToken).ConfigureAwait(false);
            cancellationToken.ThrowIfCancellationRequested();
            return Select(selected, new(null, null));
        }
        using var collection = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        var github = TrySourceAsync(UpdateSourcePreference.GitHub, preference, current, collection.Token);
        var mirror = TrySourceAsync(UpdateSourcePreference.Gitee, preference, current, collection.Token);
        var first = await Task.WhenAny(github, mirror).ConfigureAwait(false);
        var other = first == github ? mirror : github;
        var firstResult = await first.ConfigureAwait(false);
        if (firstResult.Result is not null && !other.IsCompleted)
        {
            // A usable source must not wait the complete timeout of a slow mirror.
            // Give the second host a short opportunity to return a newer version.
            await Task.WhenAny(other, Task.Delay(TimeSpan.FromSeconds(1), cancellationToken)).ConfigureAwait(false);
            if (!other.IsCompleted)
            {
                collection.Cancel();
                try { await other.ConfigureAwait(false); }
                catch (OperationCanceledException) when (collection.IsCancellationRequested) { }
                cancellationToken.ThrowIfCancellationRequested();
                return firstResult.Result;
            }
        }
        var otherResult = await other.ConfigureAwait(false);
        cancellationToken.ThrowIfCancellationRequested();
        return first == github ? Select(firstResult, otherResult) : Select(otherResult, firstResult);
    }

    private sealed record SourceCheck(UpdateCheckResult? Result, Exception? Failure);

    private async Task<SourceCheck> TrySourceAsync(UpdateSourcePreference source, UpdateSourcePreference preference, Version current,
        CancellationToken cancellationToken)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(TimeSpan.FromSeconds(8));
        try
        {
            return new(source == UpdateSourcePreference.Gitee
                ? await ReadMirrorAsync(current, preference, deadline.Token).ConfigureAwait(false)
                : await ReadGitHubAsync(current, preference, deadline.Token).ConfigureAwait(false), null);
        }
        catch (InvalidDataException failure) { return new(null, failure); }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        { return new(null, new HttpRequestException("The update source timed out.")); }
        catch (HttpRequestException failure)
        { return new(null, new HttpRequestException("The official update source could not be read.", null, failure.StatusCode)); }
        catch (IOException)
        { return new(null, new IOException("The official update source could not be read.")); }
    }

    private static UpdateCheckResult? Select(SourceCheck github, SourceCheck mirror)
    {
        if (github.Result is not { } a)
        {
            if (mirror.Result is not null) return mirror.Result;
            var failure = github.Failure ?? mirror.Failure;
            if (failure is not null) System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failure).Throw();
            return null;
        }
        if (mirror.Result is not { } b) return a;
        var comparison = Version.Parse(a.Version).CompareTo(Version.Parse(b.Version));
        if (comparison != 0) return comparison > 0 ? a : b;
        if (a.Installer is { } left && b.Installer is { } right &&
            (left.Size != right.Size || !left.Sha256.Equals(right.Sha256, StringComparison.OrdinalIgnoreCase)))
            throw new InvalidDataException("The official sources disagree on installer integrity.");
        return a.Installer is not null || b.Installer is null ? a : b;
    }

    private async Task<UpdateCheckResult?> ReadGitHubAsync(Version current, UpdateSourcePreference preference, CancellationToken token)
    {
        using var response = await UpdateSources.GetAsync(client, LatestReleaseUri, null, token).ConfigureAwait(false);
        if (response.StatusCode == HttpStatusCode.NotFound) return null;
        if (response.StatusCode == HttpStatusCode.TooManyRequests || (response.StatusCode == HttpStatusCode.Forbidden
            && (response.Headers.RetryAfter is not null || (response.Headers.TryGetValues("X-RateLimit-Remaining", out var counts) && counts.Contains("0")))))
            return await ReadGitHubFallbackAsync(current, preference, token).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
        using var release = await UpdateSources.ReadJsonAsync(response, 1024 * 1024, token).ConfigureAwait(false);
        var root = release.RootElement;
        var tag = Text(root, "tag_name");
        if (!TryStableTag(tag, out var latest) || !False(root, "draft") || !False(root, "prerelease"))
            throw new InvalidDataException("The latest release is not a stable NovaBar version.");
        var version = latest.ToString(3);
        var releaseUri = new Uri(RepositoryRoot + "/releases/tag/" + tag);
        if (Text(root, "html_url") != releaseUri.AbsoluteUri) throw new InvalidDataException("The release points outside the official repository.");
        var name = OptionalText(root, "name");
        var notes = OptionalText(root, "body");
        var assets = Assets(root);
        UpdatePackage? package = null;
        if (FindAsset(assets, "update.json") is { } asset)
        {
            var manifestUri = UpdateSources.AssetUri(version, "update.json");
            if (Text(asset, "browser_download_url") != manifestUri.AbsoluteUri)
                throw new InvalidDataException("The manifest URL is invalid.");
            using var manifestResponse = await UpdateSources.GetAsync(client, manifestUri, manifestUri, token).ConfigureAwait(false);
            manifestResponse.EnsureSuccessStatusCode();
            using var manifest = await UpdateSources.ReadJsonAsync(manifestResponse, 64 * 1024, token).ConfigureAwait(false);
            package = ReadPackage(manifest.RootElement, version, UpdateSourcePreference.GitHub, preference);
            if (FindAsset(assets, package.FileName) is not { } installer || Text(installer, "browser_download_url") != package.DownloadUri.AbsoluteUri
                || !installer.TryGetProperty("size", out var size) || !IsNumber(size) || !size.TryGetInt64(out var bytes) || bytes != package.Size)
                throw new InvalidDataException("The release and manifest disagree on the installer.");
        }
        return new(version, latest > current, name, notes, releaseUri, package);
    }

    private async Task<UpdateCheckResult?> ReadMirrorAsync(Version current, UpdateSourcePreference preference, CancellationToken token)
    {
        using var response = await UpdateSources.GetAsync(client, UpdateSources.GiteeReleases, null, token).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
        using var releases = await UpdateSources.ReadJsonAsync(response, 1024 * 1024, token).ConfigureAwait(false);
        if (releases.RootElement.ValueKind != JsonValueKind.Array || releases.RootElement.GetArrayLength() > 100)
            throw new InvalidDataException("The Gitee release list is invalid.");
        Version? latest = null;
        var selected = default(JsonElement);
        var versions = new HashSet<Version>();
        foreach (var row in releases.RootElement.EnumerateArray())
        {
            if (!False(row, "prerelease") || (row.TryGetProperty("draft", out var draft) && draft.ValueKind != JsonValueKind.False)
                || !TryStableTag(OptionalText(row, "tag_name"), out var candidate)) continue;
            if (!versions.Add(candidate)) throw new InvalidDataException("The mirror has ambiguous release versions.");
            var version = candidate.ToString(3);
            var assets = Assets(row);
            if (FindAsset(assets, "update.json") is not { } manifest || FindAsset(assets, InstallerName(version)) is not { } installer)
                continue;
            if (Text(manifest, "browser_download_url") != UpdateSources.AssetUri(version, "update.json", UpdateSourcePreference.Gitee).AbsoluteUri
                || Text(installer, "browser_download_url") != UpdateSources.AssetUri(version, InstallerName(version), UpdateSourcePreference.Gitee).AbsoluteUri)
                throw new InvalidDataException("The Gitee release has an invalid asset URL.");
            if (latest is null || candidate > latest) (latest, selected) = (candidate, row);
        }
        if (latest is null) return null;
        var displayVersion = latest.ToString(3);
        var manifestUri = UpdateSources.AssetUri(displayVersion, "update.json", UpdateSourcePreference.Gitee);
        using var manifestResponse = await UpdateSources.GetAsync(client, manifestUri, manifestUri, token).ConfigureAwait(false);
        manifestResponse.EnsureSuccessStatusCode();
        using var document = await UpdateSources.ReadJsonAsync(manifestResponse, 64 * 1024, token).ConfigureAwait(false);
        var package = ReadPackage(document.RootElement, displayVersion, UpdateSourcePreference.Gitee, preference);
        return new(displayVersion, latest > current, OptionalText(selected, "name"), OptionalText(selected, "body"),
            new Uri(UpdateSources.GiteeRoot + "/releases/tag/v" + displayVersion), package) { Source = UpdateSourcePreference.Gitee };
    }

    private async Task<UpdateCheckResult?> ReadGitHubFallbackAsync(Version current, UpdateSourcePreference preference, CancellationToken token)
    {
        var (manifestResponse, expected) = await UpdateSources.GetGitHubLatestManifestAsync(client, token).ConfigureAwait(false);
        using var response = manifestResponse;
        response.EnsureSuccessStatusCode();
        using var document = await UpdateSources.ReadJsonAsync(response, 64 * 1024, token).ConfigureAwait(false);
        var version = Text(document.RootElement, "version");
        if (!TryParseVersion(version, out var latest) || version != latest.ToString(3) || (expected is not null && expected != version))
            throw new InvalidDataException("The GitHub fallback manifest points to another release version.");
        var package = ReadPackage(document.RootElement, version, UpdateSourcePreference.GitHub, preference);
        return new(version, latest > current, "NovaBar " + version, "", new Uri(RepositoryRoot + "/releases/tag/v" + version), package);
    }

    private static UpdatePackage ReadPackage(JsonElement root, string version, UpdateSourcePreference source, UpdateSourcePreference preference)
    {
        if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("schemaVersion", out var schema) || !IsNumber(schema)
            || !schema.TryGetInt32(out var schemaNumber) || schemaNumber != 1
            || Text(root, "version") != version || !root.TryGetProperty("installer", out var installer) || installer.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException("The update manifest schema/version is invalid.");
        var fileName = InstallerName(version);
        // Both releases publish the same bytes, including the canonical GitHub URL in update.json.
        var uri = UpdateSources.AssetUri(version, fileName);
        var hash = Text(installer, "sha256");
        if (Text(installer, "fileName") != fileName || Text(installer, "url") != uri.AbsoluteUri
            || !Regex.IsMatch(hash, @"\A[0-9a-fA-F]{64}\z", RegexOptions.CultureInvariant)
            || !installer.TryGetProperty("size", out var size) || !IsNumber(size) || !size.TryGetInt64(out var bytes) || bytes <= 0 || bytes > MaximumInstallerSize)
            throw new InvalidDataException("The update installer identity or integrity metadata is invalid.");
        return new(version, fileName, UpdateSources.AssetUri(version, fileName, source), hash, bytes, source, preference);
    }

    private static bool TryStableTag(string tag, out Version version) => TryParseVersion(tag, out version) && tag == "v" + version.ToString(3);
    private static string InstallerName(string version) => $"NovaBar-{version}-win-x64-Setup.exe";
    private static bool IsNumber(JsonElement element) => element.ValueKind == JsonValueKind.Number;
    private static bool False(JsonElement root, string name) => root.ValueKind == JsonValueKind.Object
        && root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.False;

    private static JsonElement Assets(JsonElement release) => release.TryGetProperty("assets", out var assets) && assets.ValueKind == JsonValueKind.Array
        ? assets : throw new InvalidDataException("The release asset list is invalid.");

    private static JsonElement? FindAsset(JsonElement assets, string fileName)
    {
        JsonElement? found = null;
        foreach (var asset in assets.EnumerateArray())
        {
            if (OptionalText(asset, "name") != fileName) continue;
            if (found is not null) throw new InvalidDataException("The release has ambiguous attachments.");
            found = asset;
        }
        return found;
    }

    private static string Text(JsonElement root, string name)
    {
        if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty(name, out var value) || value.ValueKind != JsonValueKind.String)
            throw new InvalidDataException("The update metadata is incomplete.");
        return value.GetString()!;
    }

    private static string OptionalText(JsonElement root, string name) => root.ValueKind == JsonValueKind.Object
        && root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString()! : "";
}
