[CmdletBinding(DefaultParameterSetName='Run')]
param(
    [Parameter(Mandatory=$true,ParameterSetName='Run')][string]$BaselineExecutable,
    [Parameter(Mandatory=$true,ParameterSetName='Run')][string]$CurrentExecutable,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [int]$Samples = 30,
    [int]$Rounds = 3,
    [Parameter(Mandatory=$true,ParameterSetName='Summarize')][switch]$SummarizeOnly
)
$ErrorActionPreference = 'Stop'
if ($Samples -lt 1 -or $Rounds -lt 1) { throw 'Samples and rounds must be positive.' }
$root = Split-Path -Parent $PSScriptRoot
$output = [IO.Path]::GetFullPath($OutputDirectory)
$allowed = [IO.Path]::GetFullPath((Join-Path $root 'out')) + [IO.Path]::DirectorySeparatorChar
if (-not $output.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Performance output must be inside the project out directory.' }
$environmentPath = Join-Path $output 'environment.json'
$throughputMeaning = 'Measured-window throughput including revisits: each round spans the first measured Dispatch through the last measured Publish. Elapsed milliseconds = sum(interval_ms) - firstMeasured.interval_ms + lastMeasured.dispatch_publish_ms. Aggregate throughput is sum(added rows) / sum(round elapsed seconds), excluding preparation, warmup and final boundary detection.'
if ($SummarizeOnly) {
    $metadata = Get-Content -LiteralPath $environmentPath -Raw | ConvertFrom-Json
    if (-not $PSBoundParameters.ContainsKey('Samples')) { $Samples = [int]$metadata.samples }
    if (-not $PSBoundParameters.ContainsKey('Rounds')) { $Rounds = [int]$metadata.rounds }
    if ($Samples -lt 1 -or $Rounds -lt 1 -or $Samples -ne [int]$metadata.samples -or $Rounds -ne [int]$metadata.rounds) {
        throw 'Summary samples/rounds must match the original environment.json.'
    }
} else {
    $executables = @{
        baseline = (Resolve-Path -LiteralPath $BaselineExecutable).Path
        current = (Resolve-Path -LiteralPath $CurrentExecutable).Path
    }
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    $cpu = 'Unknown'
    try { $cpu = Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Name }
    catch {
        try { $cpu = (Get-ItemProperty -LiteralPath 'HKLM:\HARDWARE\DESCRIPTION\System\CentralProcessor\0' -Name ProcessorNameString -ErrorAction Stop).ProcessorNameString.Trim() }
        catch { Write-Warning 'CPU metadata unavailable; continuing the benchmark.' }
    }
    $metadata = [ordered]@{
        timestamp = [DateTimeOffset]::Now.ToString('o')
        samples = $Samples
        rounds = $Rounds
        warmup = 'First action marked first; next three warmup; excluded from steady-state summaries.'
        cpu = $cpu
        os = [Environment]::OSVersion.VersionString
        traceQpcFrequency = [Diagnostics.Stopwatch]::Frequency
        unavailableMetrics = 'Automatic input_* metrics have no per-action input event. Revisits (added_height=0) may omit preview and paint metrics because the image is unchanged. Missing values must be exactly -1.'
        throughputMeaning = $throughputMeaning
        rawPhaseMeaning = 'Raw capture durations include preparation, warmup, idle waits, measured actions and boundary detection; split by whether a frame was returned. Flatten is the final image export. Round 0 aggregates all rounds.'
        baselineSha256 = (Get-FileHash -LiteralPath $executables.baseline).Hash
        currentSha256 = (Get-FileHash -LiteralPath $executables.current).Hash
    }
    $metadata | ConvertTo-Json | Set-Content -LiteralPath $environmentPath -Encoding utf8
}
$traceQpcFrequency = [double]$metadata.traceQpcFrequency
if ($traceQpcFrequency -le 0 -or [double]::IsNaN($traceQpcFrequency) -or [double]::IsInfinity($traceQpcFrequency)) { throw 'Missing or invalid trace QPC frequency in environment.json.' }
$scenarios = @('immediate','smooth','delayed','whole','automatic')
$metrics = @('input_dispatch_ms','dispatch_receive_ms','receive_first_source_paint_ms',
    'first_source_paint_capture_ms','frame_stitch_wait_ms','stitch_ms','preview_ms',
    'input_publish_ms','input_paint_ms','dispatch_publish_ms','dispatch_paint_ms','interval_ms')
function Read-FiniteNumber($Value, [string]$Context) {
    $number = 0.0
    if (-not [double]::TryParse([string]$Value, [Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture, [ref]$number) -or
        [double]::IsNaN($number) -or [double]::IsInfinity($number)) { throw "Invalid numeric value in ${Context}: '$Value'." }
    return $number
}
function Get-Median([double[]]$SortedValues) {
    if (-not $SortedValues.Count) { throw 'Cannot calculate a median without samples.' }
    $middle = [int][Math]::Floor($SortedValues.Count / 2)
    if ($SortedValues.Count % 2) { return $SortedValues[$middle] }
    return ($SortedValues[$middle - 1] / 2 + $SortedValues[$middle] / 2)
}
function Get-ThroughputWindow($Actions, [string]$Implementation, [string]$Scenario, [int]$Round) {
    $ordered = @($Actions | Sort-Object { [int]$_.action })
    if (-not $ordered.Count) { throw "No measured actions for $Implementation/$Scenario/$Round." }
    $added = ($ordered | ForEach-Object { [int]$_.added_height } | Measure-Object -Sum).Sum
    $intervals = ($ordered | ForEach-Object { Read-FiniteNumber $_.interval_ms 'throughput interval' } | Measure-Object -Sum).Sum
    $elapsed = $intervals - (Read-FiniteNumber $ordered[0].interval_ms 'first measured interval') +
        (Read-FiniteNumber $ordered[-1].dispatch_publish_ms 'last measured dispatch to publish')
    if ($elapsed -le 0) { throw "Nonpositive measured throughput window for $Implementation/$Scenario/$Round." }
    return [pscustomobject]@{ implementation=$Implementation; scenario=$Scenario; round=$Round;
        first_action=$ordered[0].action; last_action=$ordered[-1].action; measured_actions=$ordered.Count;
        added_rows=$added; elapsed_ms=$elapsed; new_rows_per_second_including_revisits=1000*$added/$elapsed }
}
function Test-ActionBatch($Batch, [string]$Context) {
    foreach ($row in $Batch) {
        if ($row.scenario -notin $scenarios) { throw "${Context}: unknown scenario '$($row.scenario)'." }
        foreach ($column in (@('action','sample_kind','direction','pixels','added_height') + $metrics)) {
            if ($column -notin $row.PSObject.Properties.Name) { throw "${Context}: missing column '$column'." }
        }
    }
    foreach ($scenario in $scenarios) {
        $actions = @($Batch | Where-Object scenario -eq $scenario)
        if ($actions.Count -ne $Samples + 4) { throw "${Context}/${scenario}: expected $($Samples + 4) successful actions, got $($actions.Count)." }
        $seen = @{}
        foreach ($row in $actions) {
            $action = Read-FiniteNumber $row.action "$Context/$scenario/action"
            if ($action -ne [Math]::Floor($action) -or $action -lt 1 -or $action -gt $Samples + 4 -or $seen.ContainsKey($action)) {
                throw "${Context}/${scenario}: invalid or duplicate action '$action'."
            }
            $seen[$action] = $true
            $kind = if ($action -eq 1) { 'first' } elseif ($action -le 4) { 'warmup' } else { 'measured' }
            if ($row.sample_kind -ne $kind) { throw "${Context}/${scenario}/$action`: expected sample_kind '$kind', got '$($row.sample_kind)'." }
            $added = Read-FiniteNumber $row.added_height "$Context/$scenario/$action/added_height"
            $pixels = Read-FiniteNumber $row.pixels "$Context/$scenario/$action/pixels"
            $direction = Read-FiniteNumber $row.direction "$Context/$scenario/$action/direction"
            if ($added -lt 0 -or $added -ne [Math]::Floor($added) -or $pixels -eq 0 -or
                $pixels -ne [Math]::Floor($pixels) -or $added -gt [Math]::Abs($pixels) -or
                $direction -notin @(-1,1) -or $direction * $pixels -ge 0) { throw "${Context}/${scenario}/$action`: invalid displacement or added height." }
            foreach ($metric in $metrics) {
                $value = Read-FiniteNumber $row.$metric "$Context/$scenario/$action/$metric"
                # Automatic continuation has no physical input per action. A
                # revisit reuses the image, so no new preview/paint is required.
                $optional = ($scenario -eq 'automatic' -and $metric.StartsWith('input_')) -or
                    ($added -eq 0 -and $metric -in @('preview_ms','input_paint_ms','dispatch_paint_ms'))
                if ($value -lt 0 -and (-not $optional -or $value -ne -1)) {
                    throw "${Context}/${scenario}/$action`: required or malformed metric '$metric' is $value."
                }
                if ($metric -eq 'interval_ms' -and (($action -eq 1 -and $value -ne 0) -or ($action -gt 1 -and $value -le 0))) {
                    throw "${Context}/${scenario}/$action`: invalid dispatch interval $value."
                }
            }
        }
    }
}
function Test-ComparableWorkload($Batch, [string]$Context, [hashtable]$Reference) {
    foreach ($row in $Batch) {
        $key = "$($row.scenario)/$($row.action)"
        if (-not $Reference.ContainsKey($key)) {
            $Reference[$key] = @{ context=$Context; direction=$row.direction; pixels=$row.pixels; added_height=$row.added_height }
        } else {
            foreach ($field in @('direction','pixels','added_height')) {
                $actual = Read-FiniteNumber $row.$field "$Context/$key/$field"
                $expected = Read-FiniteNumber $Reference[$key][$field] "$($Reference[$key].context)/$key/$field"
                if ($actual -ne $expected) {
                    throw "${Context}/${key}: workload '$field' is $actual; $($Reference[$key].context) measured $expected. Performance results are not comparable."
                }
            }
        }
    }
}
# Keep raw capture/export durations separate from per-action latency. Reuse the
# original run's QPC frequency when recomputing saved traces.
$phaseSamples = [Collections.Generic.List[object]]::new()
function Read-RawPhases([string]$Path, [string]$Implementation, [int]$Round) {
    if (-not (Test-Path -LiteralPath $Path)) { Write-Warning "No raw trace at $Path; phase summary omitted for this run."; return }
    $pending = @{}
    foreach ($event in (Import-Csv -LiteralPath $Path)) {
        $stage = [int]$event.stage
        if ($stage -notin @(3,4,14,15)) { continue } # CaptureBegin/End, FlattenBegin/End.
        $phase = if ($stage -in @(3,4)) { 'capture' } else { 'flatten' }
        $key = "$($event.scenario)/$phase/$($event.action)"
        $qpc = Read-FiniteNumber $event.qpc "$Path/$key/qpc"
        if ($stage -in @(3,14)) {
            if ($pending.ContainsKey($key)) { throw "Unpaired $phase begin in $Path ($key)." }
            $pending[$key] = $qpc
        } else {
            if (-not $pending.ContainsKey($key) -or $qpc -lt $pending[$key]) { throw "Unpaired or reversed $phase end in $Path ($key)." }
            $duration = 1000 * ($qpc - $pending[$key]) / $traceQpcFrequency
            $pending.Remove($key)
            $phaseSamples.Add([pscustomobject]@{ implementation=$Implementation; round=$Round; scenario=$event.scenario;
                phase=$phase; captured=($phase -eq 'capture' -and $event.result -eq '1'); duration_ms=$duration })
        }
    }
    if ($pending.Count) { throw "Unfinished capture/export spans in $Path." }
}
$rows = @()
$referenceWorkload = @{}
for ($round = 1; $round -le $Rounds; $round++) {
    $order = if ($round % 2) { @('baseline','current') } else { @('current','baseline') }
    foreach ($implementation in $order) {
        $prefix = Join-Path $output "$implementation-$round"
        if (-not $SummarizeOnly) {
            & $executables[$implementation] scroll-performance $prefix $Samples *> "$prefix.log"
            if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath "$prefix.log"; throw "$implementation round $round failed." }
        }
        $batch = @(Import-Csv -LiteralPath "$prefix-actions.csv")
        Test-ActionBatch $batch "$implementation round $round"
        Test-ComparableWorkload $batch "$implementation round $round" $referenceWorkload
        Read-RawPhases "$prefix-trace.csv" $implementation $round
        foreach ($row in $batch) {
            $row | Add-Member -NotePropertyName implementation -NotePropertyValue $implementation
            $row | Add-Member -NotePropertyName round -NotePropertyValue $round
        }
        $rows += $batch
        if ($SummarizeOnly) { Write-Output "$implementation round $round saved CSV validation complete." }
        else { Write-Output "$implementation round $round complete; source positions and final pixels verified." }
    }
}
$rows | Export-Csv -LiteralPath (Join-Path $output 'all-actions.csv') -NoTypeInformation -Encoding utf8
$throughputWindows = foreach ($implementation in @('baseline','current')) {
    foreach ($scenario in $scenarios) {
        foreach ($round in (1..$Rounds)) {
            $actions = @($rows | Where-Object { $_.implementation -eq $implementation -and $_.scenario -eq $scenario -and
                $_.round -eq $round -and $_.sample_kind -eq 'measured' })
            Get-ThroughputWindow $actions $implementation $scenario $round
        }
    }
}
$throughputWindows | Export-Csv -LiteralPath (Join-Path $output 'throughput-windows.csv') -NoTypeInformation -Encoding utf8
$summary = foreach ($implementation in @('baseline','current')) {
    foreach ($scenario in $scenarios) {
        $measured = @($rows | Where-Object { $_.implementation -eq $implementation -and $_.scenario -eq $scenario -and $_.sample_kind -eq 'measured' })
        foreach ($metric in $metrics) {
            $values = @($measured | ForEach-Object { [double]::Parse($_.$metric,[Globalization.CultureInfo]::InvariantCulture) } | Where-Object { $_ -ge 0 } | Sort-Object)
            if ($values.Count) {
                [pscustomobject]@{ implementation=$implementation; scenario=$scenario; metric=$metric; samples=$values.Count; unavailable_samples=$measured.Count-$values.Count;
                    median=(Get-Median $values); p95=$values[[int][Math]::Ceiling($values.Count*0.95)-1] }
            } else {
                [pscustomobject]@{ implementation=$implementation; scenario=$scenario; metric=$metric; samples=0; unavailable_samples=$measured.Count; median=$null; p95=$null }
            }
        }
        $windows = @($throughputWindows | Where-Object { $_.implementation -eq $implementation -and $_.scenario -eq $scenario })
        $added = ($windows | Measure-Object -Property added_rows -Sum).Sum
        $elapsed = ($windows | Measure-Object -Property elapsed_ms -Sum).Sum
        [pscustomobject]@{ implementation=$implementation; scenario=$scenario; metric='new_rows_per_second_measured_window_including_revisits'; samples=$measured.Count; unavailable_samples=0;
            median=1000*$added/$elapsed; p95=$null }
    }
}
$summary | Export-Csv -LiteralPath (Join-Path $output 'summary.csv') -NoTypeInformation -Encoding utf8
$phaseSummary = foreach ($implementation in @('baseline','current')) {
    foreach ($scenario in $scenarios) {
        foreach ($phase in @('capture_all','capture_success','capture_no_frame','flatten')) {
            foreach ($round in (0..$Rounds)) {
                $values = @($phaseSamples | Where-Object { $_.implementation -eq $implementation -and $_.scenario -eq $scenario -and
                    ($round -eq 0 -or $_.round -eq $round) -and
                    (($phase -eq 'flatten' -and $_.phase -eq 'flatten') -or
                     ($_.phase -eq 'capture' -and ($phase -eq 'capture_all' -or ($phase -eq 'capture_success' -and $_.captured) -or ($phase -eq 'capture_no_frame' -and -not $_.captured)))) } |
                    ForEach-Object duration_ms | Sort-Object)
                if ($values.Count) {
                    [pscustomobject]@{ implementation=$implementation; scenario=$scenario; phase=$phase; round=$round; samples=$values.Count;
                        median_ms=(Get-Median $values); p95_ms=$values[[int][Math]::Ceiling($values.Count*0.95)-1] }
                }
            }
        }
    }
}
$phaseSummary | Export-Csv -LiteralPath (Join-Path $output 'raw-phase-summary.csv') -NoTypeInformation -Encoding utf8
[ordered]@{ timestamp=[DateTimeOffset]::Now.ToString('o'); summarizeOnly=[bool]$SummarizeOnly;
    samplesPerRound=$Samples; rounds=$Rounds; median='Middle sample for odd counts; average of the two central samples for even counts.';
    p95='Nearest rank: sorted sample at ceil(0.95 * count), using one-based ranks.';
    throughput=$throughputMeaning; throughputSummaryValue='The throughput row uses the median column for the aggregate ratio of total added rows to total measured elapsed seconds; it is not a median of per-round rates.' } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'summary-method.json') -Encoding utf8
Write-Output "Saved median/P95, measured-window throughput and raw phase summaries to $output"
