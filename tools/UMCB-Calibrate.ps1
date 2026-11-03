# UMCB M1.3 supervised PS4-frame calibration helper (Windows, PowerShell 5.1+)
# Talks only to this computer's Chiaki named pipe. Sends no PS4 controller input.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$form = New-Object System.Windows.Forms.Form
$form.Text = 'UMCB — Supervised Calibration (Test)'
$form.StartPosition = 'CenterScreen'
$form.Width = 475
$form.Height = 402
$form.MinimumSize = [System.Drawing.Size]::new(475,402)
$form.MaximizeBox = $false

$intro = New-Object System.Windows.Forms.Label
$intro.Text = 'Choose a running calibration-enabled Chiaki, then label the PS4 screen.'
$intro.SetBounds(15,12,435,35)
$form.Controls.Add($intro)

$processList = New-Object System.Windows.Forms.ComboBox
$processList.DropDownStyle = 'DropDownList'
$processList.SetBounds(15,50,340,25)
$form.Controls.Add($processList)

$refresh = New-Object System.Windows.Forms.Button
$refresh.Text = 'Refresh'
$refresh.SetBounds(365,50,87,27)
$form.Controls.Add($refresh)

$status = New-Object System.Windows.Forms.TextBox
$status.Multiline = $true
$status.ReadOnly = $true
$status.ScrollBars = 'Vertical'
$status.SetBounds(15,259,437,89)
$status.Text = 'No request sent yet.'
$form.Controls.Add($status)

function Refresh-ChiakiProcesses {
    $old = [string]$processList.SelectedItem
    $processList.Items.Clear()
    foreach ($p in @(Get-Process -Name chiaki -ErrorAction SilentlyContinue | Sort-Object Id)) {
        [void]$processList.Items.Add(('{0} — {1}' -f $p.Id, $p.MainWindowTitle))
    }
    if ($processList.Items.Count -eq 0) {
        $status.Text = 'No running chiaki.exe found. Launch the calibration test build first.'
    } else {
        $oldIndex = $processList.Items.IndexOf($old)
        if ($oldIndex -ge 0) { $processList.SelectedIndex = $oldIndex }
        else { $processList.SelectedIndex = 0 }
    }
}

function Send-UMCBCommand([string] $command) {
    if ($processList.SelectedIndex -lt 0) {
        $status.Text = 'Select a running Chiaki process first.'
        return $null
    }
    $processId = ([string]$processList.SelectedItem -split '\s+')[0]
    $pipeName = 'UMCB_CAL_{0}' -f $processId
    $pipe = $null
    try {
        $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', $pipeName, [System.IO.Pipes.PipeDirection]::InOut)
        $pipe.Connect(1500)
        $pipe.ReadTimeout = 3000
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($command + "`n")
        $pipe.Write($bytes,0,$bytes.Length)
        $pipe.Flush()
        $buffer = New-Object byte[] 4096
        $reply = New-Object System.Text.StringBuilder
        while ($true) {
            $read = $pipe.Read($buffer,0,$buffer.Length)
            if ($read -le 0) { break }
            [void]$reply.Append([System.Text.Encoding]::UTF8.GetString($buffer,0,$read))
            if ($reply.ToString().Contains("`n")) { break }
        }
        $response = $reply.ToString().Trim()
        if (-not $response) { $response = 'No response from Chiaki.' }
        $status.Text = $response
        return $response
    } catch {
        $status.Text = 'Could not reach the calibration pipe for Chiaki PID ' + $processId + ". Check that UMCB_CALIBRATION=1 was set before launch. `r`n" + $_.Exception.Message
        return $null
    } finally {
        if ($pipe) { $pipe.Dispose() }
    }
}

function New-CaptureButton([string]$title, [string]$label, [int]$x, [int]$y) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $title
    $b.SetBounds($x,$y,210,46)
    $b.Tag = $label
    $b.Add_Click({
        param($sender, $eventArgs)
        [void](Send-UMCBCommand ('CAPTURE ' + [string]$sender.Tag))
    })
    $form.Controls.Add($b)
}

New-CaptureButton '1. Gameplay — one frame' 'GAMEPLAY' 15 92
New-CaptureButton '2. Chat open — one frame' 'CHAT_UNFOCUSED' 242 92
New-CaptureButton '3. Cursor blinking — 12 frames' 'CHAT_CURSOR_ACTIVE' 15 150
New-CaptureButton '4. PS4 keyboard — one frame' 'PS4_KEYBOARD' 242 150

$check = New-Object System.Windows.Forms.Button
$check.Text = 'Check status'
$check.SetBounds(15,212,210,32)
$check.Add_Click({ [void](Send-UMCBCommand 'STATUS') })
$form.Controls.Add($check)

$folder = New-Object System.Windows.Forms.Button
$folder.Text = 'Open capture folder'
$folder.SetBounds(242,212,210,32)
$folder.Add_Click({
    $reply = Send-UMCBCommand 'STATUS'
    if ($reply -and $reply -match 'folder=(.+)$') {
        $path = $Matches[1]
        if (Test-Path -LiteralPath $path -PathType Container) { Start-Process -FilePath 'explorer.exe' -ArgumentList ('"' + $path + '"') }
        else { $status.Text = 'No images captured yet. Folder will be created on first successful save.' }
    }
})
$form.Controls.Add($folder)

Refresh-ChiakiProcesses
$refresh.Add_Click({ Refresh-ChiakiProcesses })
[void]$form.ShowDialog()
