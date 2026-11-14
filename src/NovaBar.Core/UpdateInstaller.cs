using System.Diagnostics;
using System.Net;
using System.Security.Cryptography;
using System.Text.RegularExpressions;

namespace NovaBar.Core;

/// <summary>Streams a bounded installer to a private random cache folder and verifies it before handoff.</summary>
public sealed class UpdateInstaller(HttpClient client, string? cacheRoot = null)
{
    private readonly string _cacheRoot = Path.GetFullPath(cacheRoot ?? Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NovaBar", "Updates"));

    public async Task<StagedUpdate> DownloadAsync(UpdatePackage package, IProgress<double>? progress = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(package);
        cancellationToken.ThrowIfCancellationRequested();
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(TimeSpan.FromMinutes(10));
        var token = deadline.Token;
        EnsurePlainAncestors(_cacheRoot);
        Directory.CreateDirectory(_cacheRoot);
        EnsurePlainAncestors(_cacheRoot);
        PruneOwnedCache();
        if (package.Preference == UpdateSourcePreference.Auto)
        {
            try { return await DownloadFromAsync(package, package.Source, progress, token).ConfigureAwait(false); }
            catch (TransportFailure) when (!token.IsCancellationRequested)
            {
                var mirror = package.Source == UpdateSourcePreference.GitHub ? UpdateSourcePreference.Gitee : UpdateSourcePreference.GitHub;
                return await DownloadFromAsync(package, mirror, progress, token).ConfigureAwait(false);
            }
        }
        return await DownloadFromAsync(package, package.Source, progress, token).ConfigureAwait(false);
    }

    private async Task<StagedUpdate> DownloadFromAsync(UpdatePackage package, UpdateSourcePreference source,
        IProgress<double>? progress, CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        var directory = Path.Combine(_cacheRoot, "update-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, package.FileName);
        FileStream? file = null;
        try
        {
            EnsurePlainAncestors(directory);
            file = new FileStream(path, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.Read, 64 * 1024, FileOptions.Asynchronous | FileOptions.SequentialScan);
            var uri = UpdateSources.AssetUri(package.Version, package.FileName, source);
            using var response = await ReadTransportAsync(async () =>
            {
                var result = await UpdateSources.GetAsync(client, uri, uri, token).ConfigureAwait(false);
                if (result.IsSuccessStatusCode) return result;
                var status = result.StatusCode;
                result.Dispose();
                throw new HttpRequestException("The official installer endpoint is unavailable.", null, status);
            }, token).ConfigureAwait(false);
            if (response.Content.Headers.ContentLength is { } contentLength && contentLength != package.Size)
                throw new InvalidDataException("The installer length differs from the manifest.");
            await using var input = await ReadTransportAsync(() => response.Content.ReadAsStreamAsync(token), token).ConfigureAwait(false);
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            var buffer = new byte[64 * 1024];
            long total = 0;
            progress?.Report(0);
            while (true)
            {
                using var idleDeadline = CancellationTokenSource.CreateLinkedTokenSource(token);
                idleDeadline.CancelAfter(TimeSpan.FromSeconds(30));
                var read = await ReadTransportAsync(() => input.ReadAsync(buffer.AsMemory(), idleDeadline.Token).AsTask(), token).ConfigureAwait(false);
                if (read == 0) break;
                total += read;
                if (total > package.Size) throw new InvalidDataException("The installer exceeds its manifest size.");
                hash.AppendData(buffer, 0, read);
                await file.WriteAsync(buffer.AsMemory(0, read), token).ConfigureAwait(false);
                progress?.Report((double)total / package.Size);
            }
            if (total != package.Size || !CryptographicOperations.FixedTimeEquals(hash.GetHashAndReset(), Convert.FromHexString(package.Sha256)))
                throw new InvalidDataException("The installer failed SHA-256/byte-count verification.");
            await file.FlushAsync(token).ConfigureAwait(false);
            token.ThrowIfCancellationRequested();
            file.Dispose();
            file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
            var staged = new StagedUpdate(path, package, file, source);
            // Recheck after replacing the write handle with the read-only launch lock.
            staged.Verify();
            return staged;
        }
        catch
        {
            file?.Dispose();
            Cleanup(path, directory);
            throw;
        }
    }

    private sealed class TransportFailure(HttpStatusCode? status = null)
        : HttpRequestException("The installer could not be read from the official update source.", null, status);

    // Only network operations can trigger fallback. A truncation, bad redirect,
    // file/hash mismatch or local disk error must not become a new download.
    private static async Task<T> ReadTransportAsync<T>(Func<Task<T>> read, CancellationToken token)
    {
        try { return await read().ConfigureAwait(false); }
        catch (HttpIOException failure) when (failure.HttpRequestError == HttpRequestError.ResponseEnded)
        { throw new InvalidDataException("The response ended before the installer was complete."); }
        catch (HttpRequestException failure) when (failure.HttpRequestError == HttpRequestError.ResponseEnded)
        { throw new InvalidDataException("The response ended before the installer was complete."); }
        catch (HttpRequestException failure)
        { throw new TransportFailure(failure.StatusCode); }
        catch (IOException)
        { throw new TransportFailure(); }
        catch (OperationCanceledException) when (!token.IsCancellationRequested)
        { throw new TransportFailure(); }
    }

    internal static void EnsurePlainAncestors(string path)
    {
        for (var directory = new DirectoryInfo(path); directory is not null; directory = directory.Parent)
            if (directory.Exists && (directory.Attributes & FileAttributes.ReparsePoint) != 0)
                throw new IOException("The update cache cannot use redirected folders.");
    }

    private void PruneOwnedCache()
    {
        // A running setup may keep its executable locked. Retry cleanup next update;
        // inspect only our own GUID folders and never recurse or touch other cache data.
        foreach (var directory in Directory.EnumerateDirectories(_cacheRoot, "update-*").Take(64))
        {
            if (!Regex.IsMatch(Path.GetFileName(directory), @"\Aupdate-[0-9a-f]{32}\z", RegexOptions.CultureInvariant)) continue;
            try
            {
                EnsurePlainAncestors(directory);
                var entries = Directory.EnumerateFileSystemEntries(directory).Take(2).ToArray();
                if (entries.Length == 0) { Directory.Delete(directory, false); continue; }
                if (entries.Length != 1 || !Regex.IsMatch(Path.GetFileName(entries[0]),
                    @"\ANovaBar-(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-win-x64-Setup\.exe\z", RegexOptions.CultureInvariant)) continue;
                Cleanup(entries[0], directory);
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }

    internal static void Cleanup(string path, string directory)
    {
        try
        {
            EnsurePlainAncestors(directory);
            if (File.Exists(path) && (File.GetAttributes(path) & FileAttributes.ReparsePoint) == 0) File.Delete(path);
            Directory.Delete(directory, false);
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
}

/// <summary>Holds the verified file open without write/delete sharing through installer launch.</summary>
public sealed class StagedUpdate : IDisposable
{
    private FileStream? _file;
    private readonly UpdatePackage _package;
    public string FilePath { get; }
    public UpdateSourcePreference Source { get; }

    internal StagedUpdate(string path, UpdatePackage package, FileStream file, UpdateSourcePreference source) =>
        (FilePath, _package, _file, Source) = (path, package, file, source);

    public void Verify()
    {
        var file = _file ?? throw new ObjectDisposedException(nameof(StagedUpdate));
        UpdateInstaller.EnsurePlainAncestors(Path.GetDirectoryName(FilePath)!);
        if ((File.GetAttributes(FilePath) & FileAttributes.ReparsePoint) != 0 || file.Length != _package.Size)
            throw new InvalidDataException("The staged update identity changed.");
        file.Position = 0;
        if (!CryptographicOperations.FixedTimeEquals(SHA256.HashData(file), Convert.FromHexString(_package.Sha256)))
            throw new InvalidDataException("The staged update failed verification.");
    }

    /// <summary>Call only after explicit user confirmation and a successful configuration flush.</summary>
    public Process StartInstaller()
    {
        Verify();
        return Process.Start(new ProcessStartInfo
        {
            FileName = FilePath,
            Arguments = "/CURRENTUSER /SILENT /NORESTART /CLOSEAPPLICATIONS /NOVABARUPDATE=1",
            UseShellExecute = false,
            WorkingDirectory = Path.GetDirectoryName(FilePath)!,
        }) ?? throw new IOException("The verified update installer did not start.");
    }

    public void Dispose()
    {
        _file?.Dispose();
        _file = null;
        UpdateInstaller.Cleanup(FilePath, Path.GetDirectoryName(FilePath)!);
    }
}
