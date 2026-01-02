# LivePlay API Test Script
# Run this while the LivePlay application is running

$baseUrl = "http://localhost:8080"

Write-Host "`n=== LivePlay API Test Suite ===" -ForegroundColor Cyan
Write-Host "Base URL: $baseUrl`n" -ForegroundColor Gray

# Test 1: Get Project Info
Write-Host "[TEST 1] Getting project info..." -ForegroundColor Yellow
try {
    $response = Invoke-RestMethod -Uri "$baseUrl/api/project/info" -Method Get
    Write-Host "✓ Project Info:" -ForegroundColor Green
    $response | ConvertTo-Json -Depth 3
} catch {
    Write-Host "✗ Failed: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host "`n---`n"

# Test 2: Get Active Cues
Write-Host "[TEST 2] Getting active cues..." -ForegroundColor Yellow
try {
    $response = Invoke-RestMethod -Uri "$baseUrl/api/cues/active" -Method Get
    Write-Host "✓ Active Cues:" -ForegroundColor Green
    $response | ConvertTo-Json -Depth 3
} catch {
    Write-Host "✗ Failed: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host "`n---`n"

# Test 3: Get Cue Info by Index (change this to match your project)
Write-Host "[TEST 3] Getting cue info for index 0,0..." -ForegroundColor Yellow
try {
    $response = Invoke-RestMethod -Uri "$baseUrl/api/cues/0,0" -Method Get
    Write-Host "✓ Cue Info:" -ForegroundColor Green
    $response | ConvertTo-Json -Depth 3
} catch {
    Write-Host "✗ Failed: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host "`n---`n"

# Test 4: Play a Cue (change index to match your project)
Write-Host "[TEST 4] Playing cue at index 0,0..." -ForegroundColor Yellow
try {
    $response = Invoke-RestMethod -Uri "$baseUrl/api/cues/0,0/play" -Method Post
    Write-Host "✓ Play Response:" -ForegroundColor Green
    $response | ConvertTo-Json -Depth 3
} catch {
    Write-Host "✗ Failed: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host "`n---`n"

# Test 5: Pause the Cue
Write-Host "[TEST 5] Pausing cue at index 0,0..." -ForegroundColor Yellow
Start-Sleep -Seconds 2
try {
    $response = Invoke-RestMethod -Uri "$baseUrl/api/cues/0,0/pause" -Method Post
    Write-Host "✓ Pause Response:" -ForegroundColor Green
    $response | ConvertTo-Json -Depth 3
} catch {
    Write-Host "✗ Failed: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host "`n---`n"

# Test 6: Resume the Cue
Write-Host "[TEST 6] Resuming cue at index 0,0..." -ForegroundColor Yellow
Start-Sleep -Seconds 1
try {
    $response = Invoke-RestMethod -Uri "$baseUrl/api/cues/0,0/resume" -Method Post
    Write-Host "✓ Resume Response:" -ForegroundColor Green
    $response | ConvertTo-Json -Depth 3
} catch {
    Write-Host "✗ Failed: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host "`n---`n"

# Test 7: Seek Within Cue
Write-Host "[TEST 7] Seeking to 10 seconds..." -ForegroundColor Yellow
try {
    $body = @{ time = 10.0 } | ConvertTo-Json
    $response = Invoke-RestMethod -Uri "$baseUrl/api/cues/0,0/seek" -Method Post -Body $body -ContentType "application/json"
    Write-Host "✓ Seek Response:" -ForegroundColor Green
    $response | ConvertTo-Json -Depth 3
} catch {
    Write-Host "✗ Failed: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host "`n---`n"

# Test 8: Update Cue Properties
Write-Host "[TEST 8] Updating cue volume to -5 dB..." -ForegroundColor Yellow
try {
    $body = @{ volume = -5.0 } | ConvertTo-Json
    $response = Invoke-RestMethod -Uri "$baseUrl/api/cues/0,0" -Method Patch -Body $body -ContentType "application/json"
    Write-Host "✓ Update Response:" -ForegroundColor Green
    $response | ConvertTo-Json -Depth 3
} catch {
    Write-Host "✗ Failed: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host "`n---`n"

# Test 9: Stop the Cue
Write-Host "[TEST 9] Stopping cue at index 0,0..." -ForegroundColor Yellow
try {
    $response = Invoke-RestMethod -Uri "$baseUrl/api/cues/0,0/stop" -Method Post
    Write-Host "✓ Stop Response:" -ForegroundColor Green
    $response | ConvertTo-Json -Depth 3
} catch {
    Write-Host "✗ Failed: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host "`n---`n"

# Test 10: Emergency Stop All
Write-Host "[TEST 10] Emergency stop all cues..." -ForegroundColor Yellow
try {
    $response = Invoke-RestMethod -Uri "$baseUrl/api/cues/stopall" -Method Post
    Write-Host "✓ Stop All Response:" -ForegroundColor Green
    $response | ConvertTo-Json -Depth 3
} catch {
    Write-Host "✗ Failed: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host "`n`n=== Test Suite Complete ===" -ForegroundColor Cyan
