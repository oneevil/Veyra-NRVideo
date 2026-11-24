# SPDX-License-Identifier: GPL-3.0-only
"""Build src/tts/GenderGraph.inc: the ONNX graph of Jaesung Huh's voice-gender-classifier
(https://huggingface.co/JaesungHuh/voice-gender-classifier, MIT; ECAPA-TDNN after Tao Ruijie)
with every weight stored as *external data inside the published model.safetensors*.

So Veyra downloads the author's own weights file, pinned by SHA-256, and carries only the
network's wiring (a few dozen kilobytes) - no converted copy of the weights is published
anywhere. ONNX Runtime reads each tensor from the safetensors file at its byte offset.

The graph starts after the feature front end (log mel spectrogram, mean-normalised), which
src/tts/Features.cpp computes: input "feats" [1, 80, frames], output "logits" [1, 2]
(0 = a man, 1 = a woman).

    python tools/voice/make_gender_graph.py model.safetensors src/tts/GenderGraph.inc

The architecture below is the original model.py (JaesungHuh/voice-gender-classifier at
49bcbecfd929, MIT) without its feature extraction.
"""

import json
import math
import struct
import sys

import onnx
import torch
from safetensors.torch import load_file
from torch import nn


class SEModule(nn.Module):
    def __init__(self, channels, bottleneck=128):
        super().__init__()
        self.se = nn.Sequential(nn.AdaptiveAvgPool1d(1), nn.Conv1d(channels, bottleneck, kernel_size=1, padding=0),
                                nn.ReLU(), nn.Conv1d(bottleneck, channels, kernel_size=1, padding=0), nn.Sigmoid())

    def forward(self, x):
        return x * self.se(x)


class Bottle2neck(nn.Module):
    def __init__(self, inplanes, planes, kernel_size, dilation, scale=8):
        super().__init__()
        width = int(math.floor(planes / scale))
        self.conv1 = nn.Conv1d(inplanes, width * scale, kernel_size=1)
        self.bn1 = nn.BatchNorm1d(width * scale)
        self.nums = scale - 1
        pad = math.floor(kernel_size / 2) * dilation
        self.convs = nn.ModuleList(nn.Conv1d(width, width, kernel_size=kernel_size, dilation=dilation, padding=pad)
                                   for _ in range(self.nums))
        self.bns = nn.ModuleList(nn.BatchNorm1d(width) for _ in range(self.nums))
        self.conv3 = nn.Conv1d(width * scale, planes, kernel_size=1)
        self.bn3 = nn.BatchNorm1d(planes)
        self.relu = nn.ReLU()
        self.width = width
        self.se = SEModule(planes)

    def forward(self, x):
        residual = x
        out = self.bn1(self.relu(self.conv1(x)))
        spx = torch.split(out, self.width, 1)
        for i in range(self.nums):
            sp = spx[i] if i == 0 else sp + spx[i]
            sp = self.bns[i](self.relu(self.convs[i](sp)))
            out = sp if i == 0 else torch.cat((out, sp), 1)
        out = torch.cat((out, spx[self.nums]), 1)
        out = self.bn3(self.relu(self.conv3(out)))
        return self.se(out) + residual


class EcapaGender(nn.Module):
    def __init__(self, C=1024):
        super().__init__()
        self.conv1 = nn.Conv1d(80, C, kernel_size=5, stride=1, padding=2)
        self.relu = nn.ReLU()
        self.bn1 = nn.BatchNorm1d(C)
        self.layer1 = Bottle2neck(C, C, kernel_size=3, dilation=2, scale=8)
        self.layer2 = Bottle2neck(C, C, kernel_size=3, dilation=3, scale=8)
        self.layer3 = Bottle2neck(C, C, kernel_size=3, dilation=4, scale=8)
        self.layer4 = nn.Conv1d(3 * C, 1536, kernel_size=1)
        self.attention = nn.Sequential(nn.Conv1d(4608, 256, kernel_size=1), nn.ReLU(), nn.BatchNorm1d(256), nn.Tanh(),
                                       nn.Conv1d(256, 1536, kernel_size=1), nn.Softmax(dim=2))
        self.bn5 = nn.BatchNorm1d(3072)
        self.fc6 = nn.Linear(3072, 192)
        self.bn6 = nn.BatchNorm1d(192)
        self.fc7 = nn.Linear(192, 2)

    def forward(self, x):  # x: log mel features [batch, 80, frames]
        x = self.bn1(self.relu(self.conv1(x)))
        x1 = self.layer1(x)
        x2 = self.layer2(x + x1)
        x3 = self.layer3(x + x1 + x2)
        x = self.relu(self.layer4(torch.cat((x1, x2, x3), dim=1)))
        t = x.size()[-1]
        global_x = torch.cat((x, torch.mean(x, dim=2, keepdim=True).repeat(1, 1, t),
                              torch.sqrt(torch.var(x, dim=2, keepdim=True).clamp(min=1e-4)).repeat(1, 1, t)), dim=1)
        w = self.attention(global_x)
        mu = torch.sum(x * w, dim=2)
        sg = torch.sqrt((torch.sum((x ** 2) * w, dim=2) - mu ** 2).clamp(min=1e-4))
        x = self.bn5(torch.cat((mu, sg), 1))
        return self.fc7(self.relu(self.bn6(self.fc6(x))))


def main(weights_path, output_path):
    state = load_file(weights_path)
    model = EcapaGender()
    model.load_state_dict(state)
    model.eval()
    onnx_path = output_path + ".onnx"
    # No constant folding: every initializer must stay the very tensor of the weights file.
    torch.onnx.export(model, torch.randn(1, 80, 120), onnx_path, input_names=["feats"], output_names=["logits"],
                      dynamic_axes={"feats": {2: "frames"}}, opset_version=17, do_constant_folding=False,
                      dynamo=False)
    graph_model = onnx.load(onnx_path)

    with open(weights_path, "rb") as f:
        header_size = struct.unpack("<Q", f.read(8))[0]
        header = json.loads(f.read(header_size))
    base = 8 + header_size
    moved = 0
    for tensor in graph_model.graph.initializer:
        entry = header.get(tensor.name)
        if entry is None:
            continue  # a small constant of the graph itself stays inline
        array = onnx.numpy_helper.to_array(tensor)
        expected = state[tensor.name].numpy()
        if entry["dtype"] != "F32" or list(array.shape) != entry["shape"] or not (array == expected).all():
            raise SystemExit(f"{tensor.name}: the exported tensor is not the stored one")
        start, end = entry["data_offsets"]
        tensor.ClearField("raw_data")
        tensor.ClearField("float_data")
        tensor.data_location = onnx.TensorProto.EXTERNAL
        del tensor.external_data[:]
        for key, value in (("location", "model.safetensors"), ("offset", str(base + start)), ("length", str(end - start))):
            item = tensor.external_data.add()
            item.key, item.value = key, value
        moved += 1
    inline = [t.name for t in graph_model.graph.initializer if t.data_location != onnx.TensorProto.EXTERNAL]
    data = graph_model.SerializeToString()
    with open(output_path, "w") as out:
        out.write("// Generated by tools/voice/make_gender_graph.py - do not edit.\n")
        out.write(f"// ECAPA-TDNN voice gender graph; {moved} weights read from model.safetensors, {len(inline)} inline.\n")
        out.write(f"inline constexpr unsigned char genderGraph[{len(data)}] = {{\n")
        for i in range(0, len(data), 24):
            out.write("    " + ", ".join(str(b) for b in data[i:i + 24]) + ",\n")
        out.write("};\n")
    print(f"{len(data)} bytes, {moved} external weights, inline: {inline}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
