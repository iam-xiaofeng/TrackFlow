"""
YOLO ONNX 导出脚本 (支持动态 batch)

用法：
    python export_onnx.py --model your_model.pt --imgsz 1024

关键: 必须使用 dynamic=True 才能支持 BatchInferenceEngine 的多帧并行推理
"""

from ultralytics import YOLO
import argparse

def export_onnx(model_path, imgsz=1024):
    """
    导出YOLO模型为ONNX格式 (支持动态batch)

    Args:
        model_path: .pt模型文件路径
        imgsz: 输入图像尺寸
    """
    print(f"Loading model from: {model_path}")
    model = YOLO(model_path)

    print(f"Exporting to ONNX with imgsz={imgsz}, dynamic=True...")

    success = model.export(
        format="onnx",
        imgsz=imgsz,
        half=False,
        simplify=True,
        opset=12,
        dynamic=True,   # 动态batch: 支持 [1,3,H,W] ~ [N,3,H,W]
        batch=4,         # 默认batch=4, dynamic=True时可变
    )

    if success:
        onnx_path = model_path.replace('.pt', '.onnx')
        print(f"Export successful: {onnx_path}")
        print(f"\nInput shape: [N, 3, {imgsz}, {imgsz}] (N=dynamic)")
        print("BatchInferenceEngine can now batch multiple frames")
        return onnx_path
    else:
        print("Export failed")
        return None

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=str, required=True, help="Path to .pt model")
    parser.add_argument("--imgsz", type=int, default=1024, help="Input image size")

    args = parser.parse_args()
    export_onnx(args.model, args.imgsz)
