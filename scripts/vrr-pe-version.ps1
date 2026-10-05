# Map a GitHub CI_VERSION to the Windows PE/MSI version. Every PE version field
# is limited to 65535, and MSI compares only major.minor.build.
#
# Workflow builds are versioned major.minor.YYYYMMDDHHmm (UTC), which can't fit,
# so the build field becomes the two-digit year and day of the year, and the
# time goes in the fourth field. It still increases with every build, and stays
# above the older VRR versions below. MSI ignores the fourth field, but
# MajorUpgrade allows same-version upgrades, so builds from the same day still
# replace each other.
#
#   6.2.YYYYMMDDHHmm          ->  6.2.YYDDD.HHmm     # 6.2.202610051530 -> 6.2.26278.1530
#
# Older VRR tags, which had to rise above stock Moonlight 6.1.0:
#
#   6.1.0-vrrN     (N <= 17)  ->  6.2.N
#   6.1.0-vrrN.P              ->  6.2.(N * 10 + P)   # vrr17.1 -> 6.2.171
#   6.1.0-vrrN     (N >= 18)  ->  6.2.(N * 10)       # vrr18   -> 6.2.180
param(
    [Parameter(Mandatory = $true)]
    [string]$CiVersion
)

if ($CiVersion -match '^(\d+)\.(\d+)\.(\d{12})$') {
    $major = [int]$Matches[1]
    $minor = [int]$Matches[2]
    $time = [datetime]::MinValue
    if (![datetime]::TryParseExact($Matches[3], 'yyyyMMddHHmm',
            [System.Globalization.CultureInfo]::InvariantCulture,
            [System.Globalization.DateTimeStyles]::None, [ref]$time)) {
        Write-Error "CI_VERSION '$CiVersion' does not end in a valid YYYYMMDDHHmm time"
        exit 1
    }
    $build = ($time.Year % 100) * 1000 + $time.DayOfYear
    $revision = $time.Hour * 100 + $time.Minute
    Write-Output "$major.$minor.$build.$revision"
    exit 0
}

if ($CiVersion -match '-vrr(\d+)\.(\d+)$') {
    Write-Output ('6.2.' + ([int]$Matches[1] * 10 + [int]$Matches[2]))
    exit 0
}

if ($CiVersion -match '-vrr(\d+)$') {
    $n = [int]$Matches[1]
    if ($n -le 17) {
        Write-Output "6.2.$n"
    } else {
        Write-Output ('6.2.' + ($n * 10))
    }
    exit 0
}

Write-Error "Cannot map CI_VERSION '$CiVersion' to a Windows PE/MSI version"
exit 1
