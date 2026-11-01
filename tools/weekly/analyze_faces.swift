// Detects faces and their capture quality in the given images.
// Usage: analyze_faces <output.json> <image>...
// Output: [{"path", "faces": [{"x","y","w","h","confidence","quality"}]}], boxes normalized, origin bottom-left.
import Foundation
import ImageIO
import Vision

let arguments = CommandLine.arguments
guard arguments.count >= 2 else {
    FileHandle.standardError.write("usage: analyze_faces <output.json> <image>...\n".data(using: .utf8)!)
    exit(2)
}
var records: [[String: Any]] = []
for path in arguments.dropFirst(2) {
    var record: [String: Any] = ["path": path, "faces": []]
    let url = URL(fileURLWithPath: path)
    guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
          let image = CGImageSourceCreateThumbnailAtIndex(source, 0, [
              kCGImageSourceCreateThumbnailFromImageAlways: true,
              kCGImageSourceThumbnailMaxPixelSize: 1200,
              kCGImageSourceCreateThumbnailWithTransform: true,
          ] as CFDictionary) else {
        record["error"] = "unreadable"
        records.append(record)
        continue
    }
    do {
        let handler = VNImageRequestHandler(cgImage: image, options: [:])
        let rectangles = VNDetectFaceRectanglesRequest()
        try handler.perform([rectangles])
        let quality = VNDetectFaceCaptureQualityRequest()
        quality.inputFaceObservations = rectangles.results ?? []
        try handler.perform([quality])
        record["faces"] = (quality.results ?? []).map { face -> [String: Any] in
            let box = face.boundingBox
            return [
                "x": box.origin.x, "y": box.origin.y, "w": box.width, "h": box.height,
                "confidence": face.confidence,
                "quality": Double(face.faceCaptureQuality ?? 0),
            ]
        }
    } catch {
        record["error"] = error.localizedDescription
    }
    records.append(record)
}
try JSONSerialization.data(withJSONObject: records, options: [.sortedKeys]).write(to: URL(fileURLWithPath: arguments[1]))
