using System.Globalization;

namespace NovaBar.Core;

/// <summary>
/// 一个 ARGB 颜色，序列化形式与 TranslucentTB 一致：<c>#AARRGGBB</c>（也接受 #RGB / #RRGGBB）。
/// </summary>
public readonly record struct ArgbColor(byte A, byte R, byte G, byte B)
{
    public static readonly ArgbColor Transparent = new(0, 0, 0, 0);
    public static readonly ArgbColor Black = new(255, 0, 0, 0);
    public static readonly ArgbColor White = new(255, 255, 255, 255);

    public double Opacity
    {
        get => A / 255d;
        init => A = (byte)Math.Clamp(Math.Round(value * 255), 0, 255);
    }

    /// <summary>按给定不透明度返回一个新颜色（保留 RGB）。</summary>
    public ArgbColor WithOpacity(double opacity) =>
        this with { A = (byte)Math.Clamp(Math.Round(opacity * 255), 0, 255) };

    public static ArgbColor FromRgb(byte r, byte g, byte b, byte a = 255) => new(a, r, g, b);

    public static ArgbColor Parse(string? text)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            return Transparent;
        }

        var s = text.Trim().TrimStart('#');

        return s.Length switch
        {
            3 => new ArgbColor(255, Nibble(s[0]), Nibble(s[1]), Nibble(s[2])),
            4 => new ArgbColor(Nibble(s[0]), Nibble(s[1]), Nibble(s[2]), Nibble(s[3])),
            6 => new ArgbColor(255, Hex(s, 0), Hex(s, 2), Hex(s, 4)),
            8 => new ArgbColor(Hex(s, 0), Hex(s, 2), Hex(s, 4), Hex(s, 6)),
            _ => throw new FormatException($"无法解析颜色: '{text}'"),
        };

        static byte Hex(string s, int i) =>
            byte.Parse(s.AsSpan(i, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);

        static byte Nibble(char c) => Hex(new string(c, 2), 0);
    }

    public static bool TryParse(string? text, out ArgbColor color)
    {
        try
        {
            color = Parse(text);
            return true;
        }
        catch
        {
            color = Transparent;
            return false;
        }
    }

    public override string ToString() => $"#{A:X2}{R:X2}{G:X2}{B:X2}";
}
