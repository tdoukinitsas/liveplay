# LivePlay HTTP API Documentation

LivePlay provides a REST API for controlling audio cues remotely. The API runs on `http://localhost:8080` by default.

## Table of Contents

- [Getting Started](#getting-started)
- [Cue Identification](#cue-identification)
- [Playback Control](#playback-control)
- [Active Cues](#active-cues)
- [Cue Information](#cue-information)
- [Cue Properties](#cue-properties)
- [Project Information](#project-information)
- [Error Handling](#error-handling)

## Getting Started

The API server starts automatically when LivePlay launches. All endpoints use JSON for request and response bodies.

**Base URL:** `http://localhost:8080`

**Content-Type:** `application/json`

## Cue Identification

Most endpoints accept a cue identifier (`:id`) which can be either:

- **UUID** - The unique identifier for a cue (e.g., `a1b2c3d4-e5f6-7890-abcd-ef1234567890`)
- **Index** - The position in the cue list (e.g., `0,2` for row 0, column 2)

The API automatically detects which format you're using.

## Playback Control

### Play a Cue

Start or restart a cue from the beginning.

```http
POST /api/cues/:id/play
```

**Examples:**

```bash
# Play by UUID
curl -X POST http://localhost:8080/api/cues/a1b2c3d4-e5f6-7890-abcd-ef1234567890/play

# Play by index
curl -X POST http://localhost:8080/api/cues/0,2/play
```

**Response:**

```json
{
  "success": true,
  "message": "Triggered item",
  "item": {
    "uuid": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
    "displayName": "Intro Music",
    "type": "audio",
    "duration": 180.5,
    "color": "#FF5733"
  }
}
```

### Stop a Cue

Stop a playing cue.

```http
POST /api/cues/:id/stop
```

**Example:**

```bash
curl -X POST http://localhost:8080/api/cues/0,2/stop
```

**Response:**

```json
{
  "success": true,
  "message": "Stopped item at index 0,2"
}
```

### Pause a Cue

Pause a playing cue (can be resumed later).

```http
POST /api/cues/:id/pause
```

**Example:**

```bash
curl -X POST http://localhost:8080/api/cues/a1b2c3d4-e5f6-7890-abcd-ef1234567890/pause
```

**Response:**

```json
{
  "success": true,
  "message": "Paused cue a1b2c3d4-e5f6-7890-abcd-ef1234567890"
}
```

### Resume a Cue

Resume a paused cue from its current position.

```http
POST /api/cues/:id/resume
```

**Example:**

```bash
curl -X POST http://localhost:8080/api/cues/0,2/resume
```

**Response:**

```json
{
  "success": true,
  "message": "Resumed cue at index 0,2"
}
```

### Seek Within a Cue

Jump to a specific time within a cue.

```http
POST /api/cues/:id/seek
```

**Request Body:**

```json
{
  "time": 45.5
}
```

**Example:**

```bash
curl -X POST http://localhost:8080/api/cues/0,2/seek \
  -H "Content-Type: application/json" \
  -d '{"time": 45.5}'
```

**Response:**

```json
{
  "success": true,
  "message": "Sought cue at index 0,2 to 45.5s"
}
```

**Notes:**
- Time must be between the cue's `inPoint` and `outPoint` (or 0 and duration if not set)
- Time is specified in seconds with up to 16 decimal places precision

## Active Cues

### Get All Active Cues

Retrieve a list of all currently playing cues.

```http
GET /api/cues/active
```

**Example:**

```bash
curl http://localhost:8080/api/cues/active
```

**Response:**

```json
{
  "success": true,
  "activeCues": [
    {
      "uuid": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
      "displayName": "Background Music",
      "duration": 240.0,
      "currentTime": 45.3,
      "volume": -3.5,
      "isDucked": false,
      "isPaused": false,
      "color": "#4CAF50",
      "inPoint": 0,
      "outPoint": 240.0,
      "currentLevel": -12.5,
      "peakLevel": -6.2
    }
  ]
}
```

### Emergency Stop All Cues

Stop all currently playing cues immediately (panic stop).

```http
POST /api/cues/stopall
```

**Example:**

```bash
curl -X POST http://localhost:8080/api/cues/stopall
```

**Response:**

```json
{
  "success": true,
  "message": "Emergency stop - all active cues stopped"
}
```

## Cue Information

### Get Cue Details

Retrieve detailed information about a specific cue.

```http
GET /api/cues/:id
```

**Example:**

```bash
curl http://localhost:8080/api/cues/0,2
```

**Response:**

```json
{
  "success": true,
  "item": {
    "uuid": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
    "displayName": "Intro Music",
    "type": "audio",
    "mediaFileName": "intro.mp3",
    "duration": 180.5,
    "inPoint": 5.0,
    "outPoint": 175.0,
    "volume": 0,
    "playFade": 2.0,
    "stopFade": 3.0,
    "crossFade": false,
    "fadeOutDuration": 3.0,
    "color": "#FF5733",
    "duckingBehavior": {
      "mode": "duck-others",
      "duckLevel": -10,
      "duckFadeIn": 1.0,
      "duckFadeOut": 1.5
    },
    "startBehavior": {
      "action": "nothing",
      "targetUuid": null,
      "targetIndex": null
    },
    "endBehavior": {
      "action": "next",
      "targetUuid": null,
      "targetIndex": null
    }
  }
}
```

## Cue Properties

### Update Cue Properties

Update one or more properties of a cue. All updates are atomic - if any field fails validation, no changes are applied.

```http
PATCH /api/cues/:id
```

**Request Body:**

Include only the properties you want to update.

```json
{
  "displayName": "New Name",
  "volume": -3.5,
  "color": "#4CAF50",
  "inPoint": 10.0,
  "outPoint": 120.0
}
```

**Example:**

```bash
curl -X PATCH http://localhost:8080/api/cues/0,2 \
  -H "Content-Type: application/json" \
  -d '{
    "displayName": "Updated Intro",
    "volume": -5.0,
    "color": "#00FF00"
  }'
```

**Response:**

```json
{
  "success": true,
  "message": "Cue updated successfully",
  "updated": {
    "displayName": "Updated Intro",
    "volume": -5.0,
    "color": "#00FF00"
  }
}
```

### Updateable Properties

#### Basic Properties

| Property | Type | Range/Format | Description |
|----------|------|--------------|-------------|
| `displayName` | string | 1-100 chars | Display name for the cue |
| `volume` | number | -60 to +10 dB | Playback volume |
| `color` | string | Hex color | Color tag (e.g., `#FF5733`) |

#### Timing Properties

| Property | Type | Range | Description |
|----------|------|-------|-------------|
| `inPoint` | number | 0 to duration | Start position in seconds (16 decimal precision) |
| `outPoint` | number | 0 to duration | End position in seconds (16 decimal precision) |
| `playFade` | number | 0 to effective duration | Fade-in duration in seconds (16 decimal precision) |
| `stopFade` | number | 0 to effective duration | Fade-out duration in seconds (16 decimal precision) |
| `fadeOutDuration` | number | 0 to 10 | Emergency stop fade duration in seconds (16 decimal precision) |

**Notes:**
- `inPoint` must be less than `outPoint`
- Effective duration = `outPoint` - `inPoint`
- All timing values support up to 16 decimal places precision

#### Crossfade

| Property | Type | Values | Description |
|----------|------|--------|-------------|
| `crossFade` | boolean | true/false | Enable crossfading |

#### Ducking Behavior

Update ducking behavior by providing a `duckingBehavior` object:

```json
{
  "duckingBehavior": {
    "mode": "duck",
    "duckLevel": -12,
    "duckFadeIn": 1.5,
    "duckFadeOut": 2.0
  }
}
```

**Ducking Modes:**

| API Value | Description |
|-----------|-------------|
| `stopall` | Stop all other cues when this plays |
| `duck` | Lower volume of other cues |
| `none` | No ducking |

**Ducking Parameters:**

| Property | Type | Range | Description |
|----------|------|-------|-------------|
| `mode` | string | See above | Ducking mode |
| `duckLevel` | number | -60 to 0 dB | Volume reduction for ducked cues |
| `duckFadeIn` | number | 0 to 10 s | Fade time when ducking starts (16 decimal precision) |
| `duckFadeOut` | number | 0 to 10 s | Fade time when ducking ends (16 decimal precision) |

#### Start Behavior

Define what happens when this cue starts:

```json
{
  "startBehavior": {
    "action": "playnext",
    "targetUuid": "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
  }
}
```

**Start Actions:**

| API Value | Description | Required Fields |
|-----------|-------------|----------------|
| `none` | Do nothing | - |
| `playnext` | Play next cue in list | - |
| `playitem` | Play specific cue | `targetUuid` |
| `playindex` | Play cue at index | `targetIndex` (array, e.g., `[0,2]`) |

#### End Behavior

Define what happens when this cue ends:

```json
{
  "endBehavior": {
    "action": "playnext"
  }
}
```

**End Actions:**

| API Value | Description | Required Fields |
|-----------|-------------|----------------|
| `none` | Do nothing | - |
| `playnext` | Play next cue | - |
| `playitem` | Go to specific cue | `targetUuid` |
| `playindex` | Go to cue at index | `targetIndex` (array) |
| `loop` | Loop this cue | - |

### Non-Updateable Properties

The following properties cannot be updated via the API:

- `uuid`
- `type`
- `mediaFileName`
- `duration`
- `waveform`

Attempting to update these will return an error.

### Validation Errors

If validation fails, you'll receive a detailed error response:

```json
{
  "success": false,
  "message": "Validation failed",
  "errors": [
    "volume must be between -60 and 10 dB",
    "inPoint (150) must be less than outPoint (120)"
  ]
}
```

## Project Information

### Get Project Info

Retrieve information about the currently loaded project.

```http
GET /api/project/info
```

**Example:**

```bash
curl http://localhost:8080/api/project/info
```

**Response:**

```json
{
  "success": true,
  "project": {
    "folderPath": "C:\\Users\\username\\Documents\\LivePlay\\MyShow",
    "projectName": "MyShow",
    "columns": 5,
    "rows": 10,
    "enableCartOnly": true
  }
}
```

## Error Handling

### HTTP Status Codes

| Code | Description |
|------|-------------|
| 200 | Success |
| 400 | Bad Request (validation error, missing parameters) |
| 404 | Not Found (cue doesn't exist) |
| 500 | Server Error (window not available, IPC error) |
| 504 | Gateway Timeout (response timeout from renderer) |

### Error Response Format

```json
{
  "success": false,
  "message": "Error description",
  "errors": ["Detailed error 1", "Detailed error 2"]
}
```

### Common Errors

**Cue Not Found:**

```json
{
  "success": false,
  "message": "Item not found at index 0,99"
}
```

**Invalid Parameter:**

```json
{
  "success": false,
  "message": "Missing \"time\" parameter in request body"
}
```

**Validation Failed:**

```json
{
  "success": false,
  "message": "Validation failed",
  "errors": [
    "volume must be between -60 and 10 dB"
  ]
}
```

## Examples

### Complete Workflow Example

```bash
# 1. Get all active cues to see what's playing
curl http://localhost:8080/api/cues/active

# 2. Get details about a specific cue
curl http://localhost:8080/api/cues/0,2

# 3. Update the cue's volume
curl -X PATCH http://localhost:8080/api/cues/0,2 \
  -H "Content-Type: application/json" \
  -d '{"volume": -5.0}'

# 4. Play the cue
curl -X POST http://localhost:8080/api/cues/0,2/play

# 5. Pause it after a few seconds
curl -X POST http://localhost:8080/api/cues/0,2/pause

# 6. Resume playback
curl -X POST http://localhost:8080/api/cues/0,2/resume

# 7. Seek to a specific time
curl -X POST http://localhost:8080/api/cues/0,2/seek \
  -H "Content-Type: application/json" \
  -d '{"time": 30.0}'

# 8. Stop the cue
curl -X POST http://localhost:8080/api/cues/0,2/stop

# 9. Emergency stop all cues
curl -X POST http://localhost:8080/api/cues/stopall
```

### JavaScript/Node.js Example

```javascript
const baseURL = 'http://localhost:8080';

// Play a cue
async function playCue(id) {
  const response = await fetch(`${baseURL}/api/cues/${id}/play`, {
    method: 'POST'
  });
  return await response.json();
}

// Update cue properties
async function updateCue(id, properties) {
  const response = await fetch(`${baseURL}/api/cues/${id}`, {
    method: 'PATCH',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify(properties)
  });
  return await response.json();
}

// Get active cues
async function getActiveCues() {
  const response = await fetch(`${baseURL}/api/cues/active`);
  return await response.json();
}

// Usage
(async () => {
  // Update and play a cue
  await updateCue('0,2', { volume: -3.5, color: '#4CAF50' });
  await playCue('0,2');

  // Check what's playing
  const active = await getActiveCues();
  console.log('Active cues:', active.activeCues);
})();
```

### Python Example

```python
import requests
import json

BASE_URL = 'http://localhost:8080'

def play_cue(cue_id):
    response = requests.post(f'{BASE_URL}/api/cues/{cue_id}/play')
    return response.json()

def update_cue(cue_id, properties):
    response = requests.patch(
        f'{BASE_URL}/api/cues/{cue_id}',
        headers={'Content-Type': 'application/json'},
        data=json.dumps(properties)
    )
    return response.json()

def get_active_cues():
    response = requests.get(f'{BASE_URL}/api/cues/active')
    return response.json()

# Usage
if __name__ == '__main__':
    # Update cue properties
    update_cue('0,2', {
        'volume': -3.5,
        'color': '#4CAF50',
        'displayName': 'My Cue'
    })

    # Play the cue
    result = play_cue('0,2')
    print(f"Played: {result['item']['displayName']}")

    # Check active cues
    active = get_active_cues()
    for cue in active['activeCues']:
        print(f"{cue['displayName']}: {cue['currentTime']:.1f}s / {cue['duration']:.1f}s")
```

## Notes

- **Volume values** support up to 2 decimal places (e.g., -3.25 dB)
- **Timer values** (inPoint, outPoint, fades) support up to 16 decimal places precision for high accuracy
- UUID detection is automatic using regex pattern matching
- Index format is always `row,column` (e.g., `0,2` for row 0, column 2)
- All time values are in seconds
- All volume/level values are in decibels (dB)
- Color values must be valid hex colors (e.g., `#FF5733`)
- The API server auto-increments the port if 8080 is already in use
- Changes made via the API are immediately synced to the project file
