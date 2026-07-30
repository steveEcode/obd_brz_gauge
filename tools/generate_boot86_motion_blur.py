#!/usr/bin/env python3

from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
PROCESSED = ROOT / "assets" / "boot86" / "processed"

RESAMPLING = getattr(Image, "Resampling", Image).BICUBIC


def scale_alpha(image: Image.Image, factor: float) -> Image.Image:
    result = image.copy()
    alpha = result.getchannel("A")
    alpha = alpha.point(
        lambda value: max(
            0,
            min(
                255,
                int(value * factor),
            ),
        )
    )
    result.putalpha(alpha)
    return result


def make_directional_rotation_blur(
    source_name: str,
    output_name: str,
) -> None:
    source_path = PROCESSED / source_name
    output_path = PROCESSED / output_name

    if not source_path.exists():
        raise SystemExit(f"缺少源图片：{source_path}")

    source = Image.open(source_path).convert("RGBA")

    if source.size != (360, 360):
        raise SystemExit(
            f"{source_name} 尺寸不是 360×360：{source.size}"
        )

    output = Image.new(
        "RGBA",
        source.size,
        (0, 0, 0, 0),
    )

    # 这是方向性旋转拖影，不是普通高斯模糊。
    #
    # 拖影预先写进 PNG：
    # ESP32 运行时仍然只绘制一张图片，
    # 不需要实时卷积，也不需要同时绘制多个残影图层。
    samples = [
        (120, 0.08),
        (90,  0.11),
        (60,  0.16),
        (30,  0.23),
        (0,   0.68),
    ]

    for angle, opacity in samples:
        rotated = source.rotate(
            angle,
            resample=RESAMPLING,
            expand=False,
            center=(180, 180),
        )

        rotated = scale_alpha(
            rotated,
            opacity,
        )

        output = Image.alpha_composite(
            output,
            rotated,
        )

    output.save(output_path)

    print(
        f"已生成：{output_path.relative_to(ROOT)} "
        f"size={output.size} mode={output.mode}"
    )


make_directional_rotation_blur(
    "boot86_gr_logo_360.png",
    "boot86_gr_logo_motion_blur_360.png",
)

make_directional_rotation_blur(
    "boot86_logo_86_360.png",
    "boot86_logo_86_motion_blur_360.png",
)
