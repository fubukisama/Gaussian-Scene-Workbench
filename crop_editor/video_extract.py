import sys
from pathlib import Path

import cv2


def safe_stem(name):
    return "".join(ch if ch.isalnum() or ch in "_.-" else "_" for ch in name) or "video"


def main():
    video_path = Path(sys.argv[1])
    images_dir = Path(sys.argv[2])
    fps = float(sys.argv[3])
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        raise RuntimeError(f"Could not open video: {video_path.name}")
    source_fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    step = max(1, int(round(source_fps / max(fps, 0.1))))
    stem = safe_stem(video_path.stem)
    written = 0
    frame_index = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        if frame_index % step == 0:
            target = images_dir / f"{stem}_{written:06d}.jpg"
            if not cv2.imwrite(str(target), frame):
                raise RuntimeError(f"Could not write frame: {target}")
            written += 1
        frame_index += 1
    cap.release()
    print(written)


if __name__ == "__main__":
    main()
