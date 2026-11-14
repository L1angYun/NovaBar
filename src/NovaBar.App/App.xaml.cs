using Microsoft.UI.Xaml;

namespace NovaBar.App;

public partial class App : Application
{
    private Window? _window;

    public App() => InitializeComponent();

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        var installerUpdate = Environment.GetCommandLineArgs().Skip(1)
            .Contains("--installer-update", StringComparer.Ordinal);
        _window = new MainWindow(installerUpdate);
        _window.Activate();
    }
}
