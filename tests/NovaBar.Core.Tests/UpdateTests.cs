using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using NovaBar.Core;

internal static class UpdateTests
{
    private const string Version = "0.4.3";
    private const string Root = "https://github.com/L1angYun/NovaBar";
    private const string Mirror = "https://gitee.com/liangyun_Liu/NovaBar";
    private const string MirrorList = "https://gitee.com/api/v5/repos/liangyun_Liu/NovaBar/releases?direction=desc&per_page=100";
    private static readonly byte[] Bytes = Encoding.UTF8.GetBytes("verified installer fixture; never executed");
    private static readonly string FileName = $"NovaBar-{Version}-win-x64-Setup.exe";
    private static readonly string InstallerUrl = $"{Root}/releases/download/v{Version}/{FileName}";
    private static readonly string ManifestUrl = $"{Root}/releases/download/v{Version}/update.json";

    public static void Run(Action<string, Action> test, Action<bool, string> assert)
    {
        test("updates compare stable semantic versions numerically", () => {
            assert(UpdateChecker.TryParseVersion("v0.10.0", out var a) && UpdateChecker.TryParseVersion("0.9.9", out var b) && a > b, "lexical version comparison");
            foreach (var bad in new[] { "0.4", "0.4.3.1", "0.4.3-beta", "0.4.3+build", "01.4.3", "0.4.3/evil", " 0.4.3" })
                assert(!UpdateChecker.TryParseVersion(bad, out _), $"unsafe/stability version accepted: {bad}");
        });
        test("updates handle repository without releases", () => {
            using var http = Client((_, _) => Task.FromResult(new HttpResponseMessage(HttpStatusCode.NotFound)));
            assert(new UpdateChecker(http).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult() is null, "no release treated as a failure/update");
        });
        test("updates read official manifest and release notes", () => {
            using var http = Fixture();
            var result = new UpdateChecker(http).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult()!;
            assert(result.IsNewer && result.Version == Version && result.ReleaseNotes == "Release fixture notes", "release presentation missing");
            assert(result.Installer?.Size == Bytes.Length && result.Installer.DownloadUri.AbsoluteUri == InstallerUrl, "manifest installer lost");
        });
        test("updates never offer an equal or older release", () => {
            using var http = Fixture();
            var result = new UpdateChecker(http).CheckAsync("0.4.4", UpdateSourcePreference.GitHub).GetAwaiter().GetResult()!;
            assert(!result.IsNewer, "downgrade offered");
        });
        test("updates reject draft and prerelease metadata", () => {
            foreach (var flag in new[] { "draft", "prerelease" }) {
                using var http = Fixture(release: Release(flag));
                Expect<InvalidDataException>(() => new UpdateChecker(http).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult());
            }
        });
        test("updates preserve release visibility without installer manifest", () => {
            using var http = Fixture(release: Release(noAssets: true));
            var result = new UpdateChecker(http).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult()!;
            assert(result.IsNewer && result.Installer is null, "missing manifest hid release or created executable package");
        });
        test("updates reject invalid or mismatched manifests", () => {
            foreach (var manifest in new[] {
                Manifest(hash: "bad"), Manifest(size: 0), Manifest(size: 536870913),
                Manifest(version: "0.4.4"), Manifest(url: "https://evil.example/setup.exe"),
                Manifest(fileName: "../setup.exe"), Manifest(schema: 2), "{}", "{\"schemaVersion\":1,\"schemaVersion\":2}"
            }) {
                using var http = Fixture(manifest: manifest);
                Expect<InvalidDataException>(() => new UpdateChecker(http).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult());
            }
        });
        test("updates reject API redirects and silently-followed redirects", () => {
            using var redirect = Client((_, _) => Task.FromResult(Redirect("https://evil.example/api")));
            Expect<InvalidDataException>(() => new UpdateChecker(redirect).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult());
            using var followed = Client((_, _) => Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) {
                Content = new StringContent(Release()), RequestMessage = new HttpRequestMessage(HttpMethod.Get, "https://evil.example/api")
            }));
            Expect<InvalidDataException>(() => new UpdateChecker(followed).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult());
        });
        test("updates bound release metadata size", () => {
            using var http = Client((_, _) => Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(new string(' ', 1048577)) }));
            Expect<InvalidDataException>(() => new UpdateChecker(http).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult());
        });
        test("updates stream and verify installer into own cache", () => WithCache(cache => {
            using var http = Fixture();
            var package = new UpdateChecker(http).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult()!.Installer!;
            string path;
            using (var staged = new UpdateInstaller(http, cache).DownloadAsync(package).GetAwaiter().GetResult()) {
                path = staged.FilePath;
                assert(File.ReadAllBytes(path).SequenceEqual(Bytes), "verified content differs");
                assert(Path.GetFullPath(path).StartsWith(Path.GetFullPath(cache) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase), "escaped cache");
                staged.Verify();
                Expect<IOException>(() => { using var write = File.Open(path, FileMode.Open, FileAccess.Write); });
            }
            assert(!File.Exists(path), "owned update file not cleaned up");
        }));
        test("updates reject corrupt and truncated installers and clean partial files", () => WithCache(cache => {
            foreach (var bytes in new[] { Bytes.Select(b => (byte)(b ^ 1)).ToArray(), Bytes[..^1], Bytes.Concat(new byte[] { 1 }).ToArray() }) {
                using var http = Fixture(installer: bytes);
                var package = new UpdateChecker(http).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult()!.Installer!;
                Expect<InvalidDataException>(() => new UpdateInstaller(http, cache).DownloadAsync(package).GetAwaiter().GetResult());
                assert(!Directory.Exists(cache) || !Directory.EnumerateFiles(cache, "*", SearchOption.AllDirectories).Any(), "partial installer retained");
            }
        }));
        test("updates permit only official HTTPS asset CDN redirects", () => WithCache(cache => {
            var cdn = "https://release-assets.githubusercontent.com/github-production-release-asset/1/fixture?signature=fixture";
            using (var http = Fixture(redirect: cdn)) {
                var package = new UpdateChecker(http).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult()!.Installer!;
                using var staged = new UpdateInstaller(http, cache).DownloadAsync(package).GetAwaiter().GetResult();
                staged.Verify();
            }
            foreach (var bad in new[] { "http://release-assets.githubusercontent.com/a", "https://release-assets.githubusercontent.com.evil.example/a", "https://user@release-assets.githubusercontent.com/a", "https://release-assets.githubusercontent.com:444/a", "https://github.com/other/repo/releases/download/v0.4.3/setup.exe" }) {
                using var http = Fixture(redirect: bad);
                var package = new UpdateChecker(http).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult()!.Installer!;
                Expect<InvalidDataException>(() => new UpdateInstaller(http, cache).DownloadAsync(package).GetAwaiter().GetResult());
            }
        }));
        test("updates bound redirect loops", () => WithCache(cache => {
            using var http = Fixture(redirect: InstallerUrl);
            var package = new UpdateChecker(http).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult()!.Installer!;
            Expect<InvalidDataException>(() => new UpdateInstaller(http, cache).DownloadAsync(package).GetAwaiter().GetResult());
        }));
        test("updates propagate check and download cancellation", () => WithCache(cache => {
            using var cancellation = new CancellationTokenSource();
            using var slow = Client(async (_, token) => { cancellation.Cancel(); await Task.Delay(Timeout.Infinite, token); return new HttpResponseMessage(); });
            Expect<OperationCanceledException>(() => new UpdateChecker(slow).CheckAsync("0.4.2", UpdateSourcePreference.GitHub, cancellation.Token).GetAwaiter().GetResult());
            using var http = Fixture();
            var package = new UpdateChecker(http).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult()!.Installer!;
            Expect<OperationCanceledException>(() => new UpdateInstaller(http, cache).DownloadAsync(package, cancellationToken: cancellation.Token).GetAwaiter().GetResult());
            assert(!Directory.Exists(cache) || !Directory.EnumerateFiles(cache, "*", SearchOption.AllDirectories).Any(), "canceled partial retained");
        }));
        test("updates cancel a streaming download and remove the partial installer", () => WithCache(cache => {
            using var metadata = Fixture();
            var package = new UpdateChecker(metadata).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult()!.Installer!;
            using var cancellation = new CancellationTokenSource();
            using var http = Client((_, _) => Task.FromResult(Ok(new StreamContent(new CancelingStream(cancellation)))));
            Expect<OperationCanceledException>(() => new UpdateInstaller(http, cache).DownloadAsync(package, cancellationToken: cancellation.Token).GetAwaiter().GetResult());
            assert(!Directory.EnumerateFiles(cache, "*", SearchOption.AllDirectories).Any(), "streaming cancellation retained partial file");
        }));
        test("updates prune only owned abandoned installer cache folders", () => WithCache(cache => {
            var abandoned = Path.Combine(cache, "update-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(abandoned);
            File.WriteAllBytes(Path.Combine(abandoned, FileName), Bytes);
            var unrelated = Path.Combine(cache, "unrelated.txt");
            File.WriteAllText(unrelated, "preserve");
            using var http = Fixture();
            var package = new UpdateChecker(http).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult()!.Installer!;
            using var staged = new UpdateInstaller(http, cache).DownloadAsync(package).GetAwaiter().GetResult();
            assert(!Directory.Exists(abandoned) && File.Exists(unrelated), "cache pruning retained installer or deleted unrelated data");
        }));
        test("startup update preference survives clone and serialization", () => {
            var settings = NovaBarSettings.CreateDefault();
            assert(!settings.Global.CheckUpdatesOnStartup, "startup network access should be optional");
            settings.Global.CheckUpdatesOnStartup = true;
            var roundtrip = JsonSerializer.Deserialize<NovaBarSettings>(JsonSerializer.Serialize(settings.Clone(), SettingsStore.JsonOptions), SettingsStore.JsonOptions)!;
            assert(roundtrip.Global.CheckUpdatesOnStartup, "preference lost");
        });
        test("automatic updates find the official mirror when GitHub is down", () => {
            using var http = Client((request, _) => Task.FromResult(request.RequestUri!.AbsoluteUri switch {
                MirrorList => Ok(new StringContent(MirrorReleaseList(Version))),
                var uri when uri == MirrorFile(Version, "update.json") => Ok(new StringContent(Manifest())),
                _ => new HttpResponseMessage(HttpStatusCode.ServiceUnavailable),
            }));
            var release = new UpdateChecker(http).CheckAsync("0.4.2").GetAwaiter().GetResult();
            assert(release is { IsNewer: true, Installer: not null } && release.Installer.DownloadUri.AbsoluteUri == MirrorFile(Version, FileName), "Gitee did not supply the installer");
        });
        test("automatic checks choose the numerically highest stable complete release", () => {
            using var http = DualFixture(mirrorList: MirrorReleaseList("0.9.0", "0.10.0", "0.11.0-preview"), mirrorVersion: "0.10.0");
            var result = new UpdateChecker(http).CheckAsync("0.9.0").GetAwaiter().GetResult()!;
            assert(result.Source == UpdateSourcePreference.Gitee && result.Version == "0.10.0" && result.IsNewer, "wrong newest version or source");
        });
        test("automatic checks reject different installer bytes for one version", () => {
            foreach (var manifest in new[] { Manifest(hash: new string('0', 64)), Manifest(size: Bytes.Length + 1) }) {
                using var http = DualFixture(mirrorManifest: manifest);
                Expect<InvalidDataException>(() => new UpdateChecker(http).CheckAsync("0.4.2").GetAwaiter().GetResult());
            }
        });
        test("automatic checks confirm both empty releases and distinguish an outage", () => {
            using var empty = Client((req, _) => Task.FromResult(req.RequestUri!.AbsoluteUri == MirrorList
                ? Ok(new StringContent("[]")) : new HttpResponseMessage(HttpStatusCode.NotFound)));
            assert(new UpdateChecker(empty).CheckAsync("0.4.2").GetAwaiter().GetResult() is null, "empty repositories reported an error");
            using var unavailable = Client((req, _) => Task.FromResult(req.RequestUri!.AbsoluteUri == MirrorList
                ? Ok(new StringContent("[]")) : new HttpResponseMessage(HttpStatusCode.ServiceUnavailable)));
            Expect<HttpRequestException>(() => new UpdateChecker(unavailable).CheckAsync("0.4.2").GetAwaiter().GetResult());
        });
        test("automatic checks give a stalled secondary source a short grace period", () => {
            using var http = DualFixture(send: async (url, token) => {
                if (url != MirrorList) return null;
                await Task.Delay(Timeout.Infinite, token);
                return null;
            });
            var start = System.Diagnostics.Stopwatch.StartNew();
            var result = new UpdateChecker(http).CheckAsync("0.4.2").GetAwaiter().GetResult()!;
            assert(result.Source == UpdateSourcePreference.GitHub && start.Elapsed < TimeSpan.FromSeconds(4), "stalled mirror delayed healthy result until its timeout");
        });
        test("explicit GitHub checks recover from API throttling without crossing sources", () => {
            var latest = Root + "/releases/latest/download/update.json";
            var quota = new Func<string, bool, HttpResponseMessage>((url, matching) => url switch {
                _ when url == UpdateChecker.LatestReleaseUri.AbsoluteUri => new HttpResponseMessage(HttpStatusCode.TooManyRequests),
                _ when url == latest => Redirect(matching ? ManifestUrl : Root + "/releases/download/v0.4.4/update.json"),
                _ when url.EndsWith("/update.json", StringComparison.Ordinal) && url.StartsWith(Root, StringComparison.Ordinal) => Ok(new StringContent(Manifest())),
                _ => throw new Exception("Quota fallback contacted an unexpected domain"),
            });
            using var okay = Client((r, _) => Task.FromResult(quota(r.RequestUri!.AbsoluteUri, true)));
            assert(new UpdateChecker(okay).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult()?.Installer is not null,
                "no verified installer after rate limiting");
            using var wrong = Client((r, _) => Task.FromResult(quota(r.RequestUri!.AbsoluteUri, false)));
            Expect<InvalidDataException>(() => new UpdateChecker(wrong).CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult());
        });
        test("Gitee manifest and installer use the official attachment redirect chain", () => WithCache(cache => {
            var manifestAttachment = Mirror + "/attach_files/123/download/update.json";
            var setupAttachment = Mirror + "/attach_files/124/download/" + FileName;
            var manifestCdn = "https://foruda.gitee.com/attach_file/456/update.json?fixture";
            var setupCdn = "https://foruda.gitee.com/attach_file/457/" + FileName.ToLowerInvariant() + "?fixture";
            using var http = Client((request, _) => Task.FromResult(request.RequestUri!.AbsoluteUri switch {
                MirrorList => Ok(new StringContent(MirrorReleaseList(Version))),
                var url when url == MirrorFile(Version, "update.json") => Redirect(manifestAttachment),
                var url when url == manifestAttachment => Redirect(manifestCdn),
                var url when url == manifestCdn => Ok(new StringContent(Manifest())),
                var url when url == MirrorFile(Version, FileName) => Redirect(setupAttachment),
                var url when url == setupAttachment => Redirect(setupCdn),
                var url when url == setupCdn => Ok(new ByteArrayContent(Bytes)),
                _ => throw new Exception("Explicit mirror contacted an unexpected URL"),
            }));
            var package = new UpdateChecker(http).CheckAsync("0.4.2", UpdateSourcePreference.Gitee).GetAwaiter().GetResult()!.Installer!;
            using var stage = new UpdateInstaller(http, cache).DownloadAsync(package).GetAwaiter().GetResult();
            assert(stage.Source == UpdateSourcePreference.Gitee && File.ReadAllBytes(stage.FilePath).SequenceEqual(Bytes), "wrong final mirror file");
        }));
        test("Gitee rejects skipped hops and wrong project or file before following", () => {
            foreach (var target in new[] { "https://foruda.gitee.com/attach_file/456/update.json?fixture",
                "https://gitee.com/other/project/attach_files/123/download/update.json", Mirror + "/attach_files/123/download/other.json",
                "http://gitee.com/liangyun_Liu/NovaBar/attach_files/123/download/update.json" }) {
                using var client = Client((request, _) => Task.FromResult(request.RequestUri!.AbsoluteUri switch {
                    MirrorList => Ok(new StringContent(MirrorReleaseList(Version))),
                    var url when url == MirrorFile(Version, "update.json") => Redirect(target),
                    _ => throw new Exception("Untrusted redirect was followed"),
                }));
                Expect<InvalidDataException>(() => new UpdateChecker(client).CheckAsync("0.4.2", UpdateSourcePreference.Gitee).GetAwaiter().GetResult());
            }
        });
        test("automatic download discards a partial transfer before a verified mirror retry", () => WithCache(cache => {
            using var metadata = DualFixture();
            var package = new UpdateChecker(metadata).CheckAsync("0.4.2").GetAwaiter().GetResult()!.Installer!;
            using var http = Client((req, _) => {
                var url = req.RequestUri!.AbsoluteUri;
                if (url == InstallerUrl) return Task.FromResult(Ok(new StreamContent(new InterruptedStream())));
                assert(url == MirrorFile(Version, FileName) && Directory.GetDirectories(cache, "update-*").Length == 1, "fallback reused the partial installer directory");
                return Task.FromResult(Ok(new ByteArrayContent(Bytes)));
            });
            using var stage = new UpdateInstaller(http, cache).DownloadAsync(package).GetAwaiter().GetResult();
            assert(stage.Source == UpdateSourcePreference.Gitee && File.ReadAllBytes(stage.FilePath).SequenceEqual(Bytes), "fallback re-used partial bytes");
        }));
        test("installer integrity failures, unsafe redirects and manual source errors never switch mirrors", () => WithCache(cache => {
            using var metadata = DualFixture();
            var checker = new UpdateChecker(metadata);
            var auto = checker.CheckAsync("0.4.2").GetAwaiter().GetResult()!.Installer!;
            var manual = checker.CheckAsync("0.4.2", UpdateSourcePreference.GitHub).GetAwaiter().GetResult()!.Installer!;
            foreach (var response in new Func<HttpResponseMessage>[] { () => Ok(new ByteArrayContent(Bytes[..^1])),
                () => Ok(new ByteArrayContent(Bytes.Select(b => (byte)(b ^ 1)).ToArray())),
                () => Redirect("https://gitee.com/another/project/setup.exe") }) {
                using var http = Client((req, _) => req.RequestUri!.AbsoluteUri == InstallerUrl ? Task.FromResult(response())
                    : throw new Exception("Integrity failure or unsafe redirect attempted fallback"));
                Expect<InvalidDataException>(() => new UpdateInstaller(http, cache).DownloadAsync(auto).GetAwaiter().GetResult());
            }
            using var network = Client((req, _) => req.RequestUri!.AbsoluteUri == InstallerUrl
                ? Task.FromResult(new HttpResponseMessage(HttpStatusCode.ServiceUnavailable)) : throw new Exception("Explicit mode tried the mirror"));
            Expect<HttpRequestException>(() => new UpdateInstaller(network, cache).DownloadAsync(manual).GetAwaiter().GetResult());
        }));
    }

    private static string MirrorFile(string version, string fileName) => $"{Mirror}/releases/download/v{version}/{fileName}";

    private static string MirrorReleaseList(params string[] versions) => JsonSerializer.Serialize(versions.Select(version => new {
        tag_name = "v" + version, prerelease = false, name = "NovaBar " + version, body = "Mirror release notes",
        assets = new[] {
            new { name = "update.json", browser_download_url = MirrorFile(version, "update.json") },
            new { name = $"NovaBar-{version}-win-x64-Setup.exe", browser_download_url = MirrorFile(version, $"NovaBar-{version}-win-x64-Setup.exe") },
        },
    }));

    private static HttpClient DualFixture(string? mirrorList = null, string? mirrorManifest = null, string mirrorVersion = Version,
        Func<string, CancellationToken, Task<HttpResponseMessage?>>? send = null) => Client(async (request, token) => {
        var url = request.RequestUri!.AbsoluteUri;
        if (send is not null && await send(url, token) is { } custom) return custom;
        if (url == UpdateChecker.LatestReleaseUri.AbsoluteUri) return Ok(new StringContent(Release()));
        if (url == ManifestUrl) return Ok(new StringContent(Manifest()));
        if (url == MirrorList) return Ok(new StringContent(mirrorList ?? MirrorReleaseList(mirrorVersion)));
        if (url == MirrorFile(mirrorVersion, "update.json")) return Ok(new StringContent(mirrorManifest ?? Manifest(version: mirrorVersion,
            fileName: $"NovaBar-{mirrorVersion}-win-x64-Setup.exe", url: $"{Root}/releases/download/v{mirrorVersion}/NovaBar-{mirrorVersion}-win-x64-Setup.exe")));
        throw new Exception("Unexpected dual-source request " + url);
    });

    private static string Release(string? enabledFlag = null, bool noAssets = false) => JsonSerializer.Serialize(new {
        tag_name = "v" + Version, name = "NovaBar " + Version, body = "Release fixture notes",
        html_url = Root + "/releases/tag/v" + Version, draft = enabledFlag == "draft", prerelease = enabledFlag == "prerelease",
        assets = noAssets ? Array.Empty<object>() : new object[] {
            new { name = "update.json", browser_download_url = ManifestUrl },
            new { name = FileName, browser_download_url = InstallerUrl, size = Bytes.Length }
        }
    });

    private static string Manifest(string? hash = null, long? size = null, string? version = null, string? url = null, string? fileName = null, int schema = 1) => JsonSerializer.Serialize(new {
        schemaVersion = schema, version = version ?? Version, installer = new {
            fileName = fileName ?? FileName, url = url ?? InstallerUrl,
            sha256 = hash ?? Convert.ToHexString(SHA256.HashData(Bytes)), size = size ?? Bytes.Length
        }
    });

    private static HttpClient Fixture(string? release = null, string? manifest = null, byte[]? installer = null, string? redirect = null) => Client((request, _) => {
        var url = request.RequestUri!.AbsoluteUri;
        if (url == UpdateChecker.LatestReleaseUri.AbsoluteUri) return Task.FromResult(Ok(new StringContent(release ?? Release())));
        if (url == ManifestUrl) return Task.FromResult(Ok(new StringContent(manifest ?? Manifest())));
        if (url == InstallerUrl && redirect is not null) return Task.FromResult(Redirect(redirect));
        if (url == InstallerUrl || url.StartsWith("https://release-assets.githubusercontent.com/", StringComparison.Ordinal))
            return Task.FromResult(Ok(new ByteArrayContent(installer ?? Bytes)));
        throw new Exception("Unexpected outbound URL: " + url);
    });

    private static HttpResponseMessage Ok(HttpContent content) => new(HttpStatusCode.OK) { Content = content };
    private static HttpResponseMessage Redirect(string url) { var response = new HttpResponseMessage(HttpStatusCode.Found); response.Headers.Location = new Uri(url); return response; }
    private static HttpClient Client(Func<HttpRequestMessage, CancellationToken, Task<HttpResponseMessage>> send) => new(new Handler(send));
    private sealed class Handler(Func<HttpRequestMessage, CancellationToken, Task<HttpResponseMessage>> send) : HttpMessageHandler
    {
        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken token) {
            var response = await send(request, token); response.RequestMessage ??= request; return response;
        }
    }
    private sealed class CancelingStream(CancellationTokenSource cancellation) : MemoryStream(Bytes)
    {
        public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken token = default)
        {
            cancellation.Cancel();
            token.ThrowIfCancellationRequested();
            return base.ReadAsync(buffer, token);
        }
    }
    private sealed class InterruptedStream() : MemoryStream(Bytes)
    {
        public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default) =>
            Position == 0 ? base.ReadAsync(buffer[..4], cancellationToken) : ValueTask.FromException<int>(new IOException("fixture network disconnected"));
    }
    private static void Expect<T>(Action action) where T : Exception { try { action(); } catch (T) { return; } throw new Exception("Expected " + typeof(T).Name); }
    private static void WithCache(Action<string> test) {
        var cache = Path.Combine(Path.GetTempPath(), "NovaBar-update-tests-" + Guid.NewGuid().ToString("N"));
        try { test(cache); } finally { if (Directory.Exists(cache)) Directory.Delete(cache, true); }
    }
}
