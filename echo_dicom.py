import math
import re
from decimal import Decimal
from itertools import accumulate

import av
import numpy as np
from pydicom import dcmread
from pydicom.pixels import apply_color_lut, apply_modality_lut, apply_voi_lut, iter_pixels
from pydicom.pixels.utils import get_expected_length
from pydicom.tag import Tag
from pydicom.uid import DeflatedExplicitVRLittleEndian
from pydicom.values import convert_DS_string


US_CINE_SOP_CLASSES = {"1.2.840.10008.5.1.4.1.1.3", "1.2.840.10008.5.1.4.1.1.3.1"}
FRAME_TIME = Tag(0x0018, 0x1063)
FRAME_TIME_VECTOR = Tag(0x0018, 0x1065)
PIXEL_DATA = Tag(0x7FE0, 0x0010)
COLOR_PHOTOMETRICS = {"RGB", "YBR_FULL", "YBR_FULL_422", "YBR_RCT", "YBR_ICT"}


def frame_timeline(header):
    count = int(header.NumberOfFrames)
    pointer = header.FrameIncrementPointer
    pointers = (Tag(pointer),) if isinstance(pointer, int) else tuple(Tag(value) for value in pointer)
    if pointers == (FRAME_TIME,):
        interval = Decimal(str(header.FrameTime))
        if not interval.is_finite() or interval <= 0:
            raise ValueError("DICOM FrameTime 必须为有限正数")
        return "FrameTime", tuple(index * interval for index in range(count)), count * interval
    if pointers == (FRAME_TIME_VECTOR,):
        vector = header.FrameTimeVector
        if isinstance(vector, bytes):
            vector = convert_DS_string(vector, header.file_meta.TransferSyntaxUID.is_little_endian)
        increments = tuple(Decimal(str(value)) for value in vector)
        if len(increments) != count or not increments or increments[0] != 0:
            raise ValueError("DICOM FrameTimeVector 需要与帧数一致，首项为 0")
        if any(not value.is_finite() or value < 0 for value in increments) or not any(value > 0 for value in increments):
            raise ValueError("DICOM FrameTimeVector 的增量必须非负，且至少有一项为正")
        return "FrameTimeVector", tuple(accumulate(increments)), None
    raise ValueError(f"DICOM FrameIncrementPointer 未指向支持的帧时间字段：{pointers}")


def monochrome_rgb(array, header):
    bits = int(header.BitsStored)
    signed = int(header.PixelRepresentation) == 1
    low = -(1 << (bits - 1)) if signed else 0
    high = (1 << (bits - int(signed))) - 1
    modality = apply_modality_lut(array, header)
    if header.get("ModalityLUTSequence"):
        low, high = 0, (1 << int(header.ModalityLUTSequence[0].LUTDescriptor[2])) - 1
    else:
        low, high = sorted(apply_modality_lut(np.array([low, high], dtype=np.float64), header))
    if header.get("VOILUTSequence"):
        low, high = 0, (1 << int(header.VOILUTSequence[0].LUTDescriptor[2])) - 1
    if high <= low:
        raise ValueError("DICOM 灰度映射范围无效")
    displayed = apply_voi_lut(modality, header)
    gray = np.rint(np.clip((displayed.astype(np.float64) - low) / (high - low), 0, 1) * 255).astype(np.uint8)
    if header.PhotometricInterpretation == "MONOCHROME1":
        gray = 255 - gray
    return np.repeat(gray[..., None], 3, axis=-1)


def rgb_pixels(array, header):
    interpretation = header.PhotometricInterpretation
    if interpretation == "PALETTE COLOR":
        rgb = apply_color_lut(array, header)[..., :3]
        if rgb.dtype == np.uint16:
            return ((rgb.astype(np.uint32) * 255 + 32767) // 65535).astype(np.uint8)
        return rgb
    if interpretation in {"MONOCHROME1", "MONOCHROME2"}:
        return monochrome_rgb(array, header)
    if interpretation in COLOR_PHOTOMETRICS and array.dtype == np.uint8 and array.ndim == 3 and array.shape[-1] == 3:
        return array
    raise ValueError(f"不支持的 DICOM 像素格式：{interpretation}，{array.dtype}，{array.shape}")


def color_flow_regions(header):
    return [(int(region.RegionLocationMinX0), int(region.RegionLocationMinY0),
             int(region.RegionLocationMaxX1) + 1, int(region.RegionLocationMaxY1) + 1)
            for region in header.get("SequenceOfUltrasoundRegions", [])
            if int(region.get("RegionSpatialFormat", 0)) in {1, 2} and int(region.get("RegionDataType", 0)) == 2]


def color_counts(frame, resized, regions):
    raw = frame.to_ndarray(format="rgb24")
    output = np.asarray(resized)
    before = np.ptp(raw, axis=-1) >= 8
    after = np.ptp(output, axis=-1) >= 8
    scale = resized.width / max(frame.width, frame.height)
    width, height = (max(1, round(value * scale)) for value in (frame.width, frame.height))
    left, top = (resized.width - width) // 2, (resized.height - height) // 2
    flow_before = np.zeros(before.shape, dtype=bool)
    flow_after = np.zeros(after.shape, dtype=bool)
    for x0, y0, x1, y1 in regions:
        x0, x1 = max(0, x0), min(frame.width, x1)
        y0, y1 = max(0, y0), min(frame.height, y1)
        if x0 < x1 and y0 < y1:
            flow_before[y0:y1, x0:x1] = True
            flow_after[top + math.floor(y0 * height / frame.height):top + math.ceil(y1 * height / frame.height),
                       left + math.floor(x0 * width / frame.width):left + math.ceil(x1 * width / frame.width)] = True
    return {"rgb_color_pixels_before": int(before.sum()), "rgb_color_pixels_after": int(after.sum()),
            "flow_color_pixels_before": int((before & flow_before).sum()),
            "flow_color_pixels_after": int((after & flow_after).sum())}


class DicomCine:
    def __init__(self, path, header=None):
        self.path = path
        self.header = dcmread(path, stop_before_pixels=True) if header is None else header
        header = self.header
        sop = str(header.get("SOPClassUID", ""))
        if sop not in US_CINE_SOP_CLASSES or str(header.get("Modality", "")) != "US":
            raise ValueError(f"{path.name} 需要是二维超声多帧 DICOM，当前 SOP Class UID 为 {sop}")
        self.frame_count = int(header.get("NumberOfFrames", 0))
        if self.frame_count < 2:
            raise ValueError(f"{path.name} 没有至少两帧")
        image_type = header.get("ImageType", [])
        image_mode = str(image_type[3]) if not isinstance(image_type, str) and len(image_type) > 3 else ""
        self.image_mode = int(image_mode, 16) if re.fullmatch(r"[0-9a-fA-F]{4}", image_mode) else None
        if self.image_mode is not None and self.image_mode & 0x0400:
            raise ValueError(f"{path.name} 的帧为空间切片，不能作为时间序列")
        if "FrameIncrementPointer" not in header:
            raise ValueError(f"{path.name} 缺少 FrameIncrementPointer，无法核对原始帧时间")
        self.timing_source, self.offsets_ms, self.duration_ms = frame_timeline(header)
        self.transfer_syntax = header.file_meta.TransferSyntaxUID
        if not self.transfer_syntax.is_compressed and self.transfer_syntax != DeflatedExplicitVRLittleEndian:
            pixel_header = dcmread(path, defer_size=1024, specific_tags=[PIXEL_DATA])
            pixel_data = pixel_header.get_item(PIXEL_DATA, keep_deferred=True)
            if (pixel_data is None or pixel_data.length < get_expected_length(header)
                    or pixel_data.value_tell + pixel_data.length > path.stat().st_size):
                raise ValueError(f"{path.name} 的 DICOM Pixel Data 缺失或被截断")
        self.color_regions = color_flow_regions(header)

    def timed_indices(self):
        return ((offset, index) for index, offset in enumerate(self.offsets_ms))

    def decode(self, indices):
        indices = tuple(indices)
        frames = iter_pixels(self.path, indices=indices, raw=False)
        for index, pixels in zip(indices, frames, strict=True):
            rgb = rgb_pixels(pixels, self.header)
            if rgb.dtype != np.uint8 or rgb.shape != (int(self.header.Rows), int(self.header.Columns), 3):
                raise ValueError(f"{self.path.name} 第 {index + 1} 帧 RGB 尺寸或数据类型异常")
            yield av.VideoFrame.from_ndarray(np.ascontiguousarray(rgb), format="rgb24")

    def timing_report(self):
        intervals = [after - before for before, after in zip(self.offsets_ms, self.offsets_ms[1:])]
        return {
            "source_frames": self.frame_count,
            "timing_source": self.timing_source,
            "first_frame_ms": float(self.offsets_ms[0]),
            "last_frame_ms": float(self.offsets_ms[-1]),
            "min_frame_interval_ms": float(min(intervals)),
            "max_frame_interval_ms": float(max(intervals)),
            "transfer_syntax": self.transfer_syntax.name,
            "photometric_interpretation": self.header.PhotometricInterpretation,
            "dicom_color_flow_regions": len(self.color_regions),
            "dicom_color_doppler_mode": None if self.image_mode is None else bool(self.image_mode & 0x0010),
            "ultrasound_color_data_present": self.header.get("UltrasoundColorDataPresent"),
        }
