# 3DGS Editor v0.1.9

## Changes

- Moves right-button and middle-button viewport panning out of TrackballControls and into the app event layer.
- Uses OrbitControls-style screen-space panning that translates the camera and target together without changing camera distance.
- Keeps left-button rotate and wheel zoom on the existing TrackballControls path.
- Updates the TrackballControls import cache key so packaged and desktop builds load the patched module.

## Notes

- Restart the editor after updating. A browser refresh is not enough when Electron has cached the old module graph.
