using System.Drawing;

namespace BeDD.Gui;

// Where the tiles go: a grid of 16:9 tiles (the games' shape) under a caption each, as large as the
// area allows, centred. Pure functions, so they are tested without a window.
internal static class TileLayout
{
    public readonly record struct Tile(Rectangle Bounds, Rectangle Caption, Rectangle Body);

    public static Tile[] Arrange(Rectangle area, int count, int pad, int caption)
    {
        if (count <= 0)
            return [];
        int bestCols = 1;
        double bestScale = double.NegativeInfinity;
        for (int cols = 1; cols <= count; cols++)
        {
            int rows = (count + cols - 1) / cols;
            double w = (area.Width - pad * (cols + 1)) / (double)cols;
            double h = (area.Height - pad * (rows + 1)) / (double)rows - caption;
            double scale = Math.Min(w / 16, h / 9);
            if (scale > bestScale)
            {
                bestScale = scale;
                bestCols = cols;
            }
        }
        int rowsUsed = (count + bestCols - 1) / bestCols;
        int tileW = Math.Max(1, (int)(bestScale * 16));
        int tileH = Math.Max(1, (int)(bestScale * 9)) + caption;
        int left = area.Left + Math.Max(pad, (area.Width - (tileW * bestCols + pad * (bestCols - 1))) / 2);
        int top = area.Top + Math.Max(pad, (area.Height - (tileH * rowsUsed + pad * (rowsUsed - 1))) / 2);
        var tiles = new Tile[count];
        for (int i = 0; i < count; i++)
        {
            int x = left + (i % bestCols) * (tileW + pad);
            int y = top + (i / bestCols) * (tileH + pad);
            tiles[i] = new Tile(new Rectangle(x, y, tileW, tileH), new Rectangle(x, y, tileW, caption),
                Rectangle.FromLTRB(x, y + caption, x + tileW, y + tileH));
        }
        return tiles;
    }

    // The source's aspect ratio (16:9 until known), as large as fits, centred in the box.
    public static Rectangle Fit(Rectangle box, Size source)
    {
        double aspect = source.Width > 0 && source.Height > 0 ? source.Width / (double)source.Height : 16 / 9.0;
        int w = box.Width, h = (int)(box.Width / aspect);
        if (h > box.Height)
        {
            h = box.Height;
            w = (int)(box.Height * aspect);
        }
        w = Math.Max(0, w);
        h = Math.Max(0, h);
        return new Rectangle(box.Left + (box.Width - w) / 2, box.Top + (box.Height - h) / 2, w, h);
    }
}
