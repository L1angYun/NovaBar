using System.Net;
using System.Globalization;
using System.Text.Json;

namespace NovaBar.Core;

internal static class UpdateSources
{
    internal const string GiteeRoot = "https://gitee.com/liangyun_Liu/NovaBar";
    internal static readonly Uri GiteeReleases = new("https://gitee.com/api/v5/repos/liangyun_Liu/NovaBar/releases?direction=desc&per_page=100");
    private static readonly Uri GitHubLatestManifest = new(UpdateChecker.RepositoryRoot + "/releases/latest/download/update.json");

    internal static Uri AssetUri(string version, string fileName, UpdateSourcePreference source = UpdateSourcePreference.GitHub) =>
        new($"{(source == UpdateSourcePreference.Gitee ? GiteeRoot : UpdateChecker.RepositoryRoot)}/releases/download/v{version}/{fileName}");

    internal static async Task<HttpResponseMessage> GetAsync(HttpClient client, Uri initial, Uri? asset, CancellationToken token) =>
        (await SendAsync(client, initial, asset, token).ConfigureAwait(false)).Response;

    internal static Task<(HttpResponseMessage Response, string? Version)> GetGitHubLatestManifestAsync(HttpClient client, CancellationToken token) =>
        SendAsync(client, GitHubLatestManifest, GitHubLatestManifest, token);

    private static async Task<(HttpResponseMessage Response, string? Version)> SendAsync(HttpClient client, Uri initial, Uri? asset, CancellationToken token)
    {
        var uri = initial;
        var gitee = asset?.Host == "gitee.com";
        var githubLatest = asset == GitHubLatestManifest;
        var previous = FileLocation.Release;
        string? version = null;
        for (var redirects = 0; ; redirects++)
        {
            token.ThrowIfCancellationRequested();
            if (asset is not null)
            {
                var location = ValidateAssetUri(uri, asset, githubLatest, ref version);
                if ((redirects == 0 && location != FileLocation.Release)
                    || (gitee && redirects > 0 && (previous, location) is not
                        ((FileLocation.Release, FileLocation.Attachment) or (FileLocation.Attachment, FileLocation.Cdn))))
                    throw new InvalidDataException("An update redirect skipped a required official endpoint.");
                previous = location;
            }
            else if (uri != UpdateChecker.LatestReleaseUri && uri != GiteeReleases)
                throw new InvalidDataException("Unexpected metadata endpoint.");
            using var request = new HttpRequestMessage(HttpMethod.Get, uri);
            request.Headers.UserAgent.ParseAdd("NovaBar-Updater/1.0");
            request.Headers.Accept.ParseAdd(asset is not null ? "application/octet-stream"
                : uri == UpdateChecker.LatestReleaseUri ? "application/vnd.github+json" : "application/json");
            if (uri == UpdateChecker.LatestReleaseUri) request.Headers.Add("X-GitHub-Api-Version", "2022-11-28");
            using var headersDeadline = CancellationTokenSource.CreateLinkedTokenSource(token);
            headersDeadline.CancelAfter(TimeSpan.FromSeconds(15));
            HttpResponseMessage response;
            try { response = await client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, headersDeadline.Token).ConfigureAwait(false); }
            catch (HttpRequestException e) { throw new HttpRequestException(e.HttpRequestError, "The official update request failed.", null, e.StatusCode); }
            try
            {
                if (response.RequestMessage?.RequestUri is not { } actual || actual.AbsoluteUri != uri.AbsoluteUri)
                    throw new InvalidDataException("An update request followed an unchecked redirect.");
                if (response.StatusCode is not (HttpStatusCode.MovedPermanently or HttpStatusCode.Found or HttpStatusCode.SeeOther
                    or HttpStatusCode.TemporaryRedirect or HttpStatusCode.PermanentRedirect)) return (response, version);
                if (asset is null || redirects >= 5 || response.Headers.Location is not { } target)
                    throw new InvalidDataException("The update redirect is invalid.");
                if (!Uri.TryCreate(uri, target, out var redirected)) throw new InvalidDataException("The update redirect is invalid.");
                uri = redirected;
            }
            catch { response.Dispose(); throw; }
            response.Dispose();
        }
    }

    private enum FileLocation { Release, Attachment, Cdn }

    private static FileLocation ValidateAssetUri(Uri uri, Uri expected, bool githubLatest, ref string? observedVersion)
    {
        if (!uri.IsAbsoluteUri || uri.Scheme != Uri.UriSchemeHttps || uri.Port != 443 || uri.UserInfo.Length != 0 || uri.Fragment.Length != 0)
            throw new InvalidDataException("The update location is not trusted HTTPS.");
        if (uri.AbsoluteUri == expected.AbsoluteUri) return FileLocation.Release;
        if (expected.Host == "gitee.com")
        {
            var fileName = expected.Segments[^1];
            if (uri.Host == "gitee.com" && uri.Query.Length == 0
                && AttachmentPath(uri.AbsolutePath, "/liangyun_Liu/NovaBar/attach_files/", fileName, true))
                return FileLocation.Attachment;
            if (uri.Host == "foruda.gitee.com" && AttachmentPath(uri.AbsolutePath, "/attach_file/", fileName, false))
                return FileLocation.Cdn;
        }
        else
        {
            if (uri.Host == "release-assets.githubusercontent.com"
                && uri.AbsolutePath.StartsWith("/github-production-release-asset/", StringComparison.Ordinal)) return FileLocation.Cdn;
            const string prefix = "/L1angYun/NovaBar/releases/download/";
            if (githubLatest && uri.Host == "github.com" && uri.Query.Length == 0 && uri.AbsolutePath.StartsWith(prefix, StringComparison.Ordinal))
            {
                var parts = uri.AbsolutePath[prefix.Length..].Split('/');
                if (parts.Length == 2 && parts[1] == "update.json" && UpdateChecker.TryParseVersion(parts[0], out var version)
                    && parts[0] == "v" + version.ToString(3))
                {
                    var stringVersion = version.ToString(3);
                    if (observedVersion is not null && observedVersion != stringVersion)
                        throw new InvalidDataException("The redirect changed the selected release version.");
                    observedVersion = stringVersion;
                    return FileLocation.Release;
                }
            }
        }
        throw new InvalidDataException("The update redirect is outside the official release/CDN endpoints.");
    }

    private static bool AttachmentPath(string path, string prefix, string fileName, bool repository)
    {
        if (!path.StartsWith(prefix, StringComparison.Ordinal)) return false;
        var parts = path[prefix.Length..].Split('/');
        return parts.Length == (repository ? 3 : 2)
            && ulong.TryParse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture, out var id) && id != 0
            && (!repository || parts[1] == "download")
            && (parts[^1] == fileName || (!repository && parts[^1] == fileName.ToLowerInvariant()));
    }

    internal static async Task<JsonDocument> ReadJsonAsync(HttpResponseMessage response, int maximumBytes, CancellationToken token)
    {
        if (response.Content.Headers.ContentLength is { } length && length > maximumBytes)
            throw new InvalidDataException("The update metadata exceeds its size limit.");
        await using var stream = await response.Content.ReadAsStreamAsync(token).ConfigureAwait(false);
        using var bytes = new MemoryStream();
        var buffer = new byte[16 * 1024];
        int read;
        while ((read = await stream.ReadAsync(buffer, token).ConfigureAwait(false)) > 0)
        {
            if (bytes.Length + read > maximumBytes) throw new InvalidDataException("The update metadata exceeds its size limit.");
            bytes.Write(buffer, 0, read);
        }
        JsonDocument? document = null;
        try
        {
            document = JsonDocument.Parse(bytes.ToArray(), new JsonDocumentOptions { MaxDepth = 16 });
            RejectDuplicateProperties(document.RootElement);
            return document;
        }
        catch (JsonException) { document?.Dispose(); throw new InvalidDataException("The update metadata is invalid JSON."); }
        catch { document?.Dispose(); throw; }
    }

    private static void RejectDuplicateProperties(JsonElement node)
    {
        if (node.ValueKind == JsonValueKind.Object)
        {
            var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (var property in node.EnumerateObject())
            {
                if (!names.Add(property.Name)) throw new InvalidDataException("The update metadata has duplicate fields.");
                RejectDuplicateProperties(property.Value);
            }
        }
        else if (node.ValueKind == JsonValueKind.Array)
            foreach (var element in node.EnumerateArray()) RejectDuplicateProperties(element);
    }
}
