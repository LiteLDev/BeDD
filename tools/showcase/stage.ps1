# Backdrop for a showcase take (record.ps1): a plain "desktop wallpaper" over the recorded region,
# so the video shows the games and the terminal and nothing else of the real desktop. Shown topmost
# without taking the focus; record.ps1 kills this process after the take.
param([int]$X, [int]$Y, [int]$W, [int]$H)

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Namespace LhiStage -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint f);
[DllImport("user32.dll")] public static extern IntPtr SetWindowLongPtr(IntPtr h, int i, IntPtr v);
[DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
'@
[LhiStage.Win]::SetProcessDpiAwarenessContext([IntPtr]::new(-4)) | Out-Null

# Wallpaper: a dark blue diagonal gradient with a soft glow.
$bmp = New-Object System.Drawing.Bitmap $W, $H
$g = [System.Drawing.Graphics]::FromImage($bmp)
$r = New-Object System.Drawing.Rectangle 0, 0, $W, $H
$b = New-Object System.Drawing.Drawing2D.LinearGradientBrush $r, ([System.Drawing.Color]::FromArgb(36, 64, 104)), ([System.Drawing.Color]::FromArgb(18, 24, 42)), 60.0
$g.FillRectangle($b, $r)
$path = New-Object System.Drawing.Drawing2D.GraphicsPath
$path.AddEllipse([int](-$W / 4), [int](-$H / 3), $W, $H)
$pb = New-Object System.Drawing.Drawing2D.PathGradientBrush $path
$pb.CenterColor = [System.Drawing.Color]::FromArgb(70, 110, 170, 230)
$pb.SurroundColors = @([System.Drawing.Color]::FromArgb(0, 110, 170, 230))
$g.FillPath($pb, $path)
$g.Dispose()

$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = 'None'; $form.ShowInTaskbar = $false; $form.StartPosition = 'Manual'
$form.Bounds = New-Object System.Drawing.Rectangle $X, $Y, $W, $H
$form.BackgroundImage = $bmp; $form.BackgroundImageLayout = 'Stretch'
$h = $form.Handle   # creates the window without showing it
$GWL_EXSTYLE = -20
$ex = [LhiStage.Win]::GetWindowLongPtr($h, $GWL_EXSTYLE).ToInt64() -bor 0x08000000 -bor 0x80   # NOACTIVATE, TOOLWINDOW
[LhiStage.Win]::SetWindowLongPtr($h, $GWL_EXSTYLE, [IntPtr]::new($ex)) | Out-Null
# Topmost, shown, not activated.
[LhiStage.Win]::SetWindowPos($h, [IntPtr]::new(-1), $X, $Y, $W, $H, 0x0040 -bor 0x0010) | Out-Null
[System.Windows.Forms.Application]::Run()
