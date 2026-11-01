// 将照片坐标一次性反查为中文地址字段。
// 输入：每行“图库标识<TAB>纬度<TAB>经度”；输出：JSON 对象 {图库标识: 地址字段}。
import CoreLocation
import Foundation

let args = CommandLine.arguments
guard args.count == 3 else {
    FileHandle.standardError.write("用法: geocode_places <坐标.tsv> <输出.json>\n".data(using: .utf8)!)
    exit(2)
}

var coordinates: [(String, CLLocation)] = []
for line in try String(contentsOfFile: args[1], encoding: .utf8).split(separator: "\n") {
    let parts = line.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
    guard parts.count == 3, let lat = Double(parts[1]), let lon = Double(parts[2]) else { continue }
    coordinates.append((parts[0], CLLocation(latitude: lat, longitude: lon)))
}

func fields(_ p: CLPlacemark) -> [String: Any] {
    return [
        "iso": p.isoCountryCode ?? "",
        "country": p.country ?? "",
        "province": p.administrativeArea ?? "",
        "city": p.locality ?? "",
        "district": p.subAdministrativeArea ?? "",
        "area": p.subLocality ?? "",
        "name": p.name ?? "",
        "interest": p.areasOfInterest ?? [],
    ]
}

let geocoder = CLGeocoder()
let locale = Locale(identifier: "zh_CN")
var cache: [String: [String: Any]] = [:]
var result: [String: [String: Any]] = [:]

for (index, item) in coordinates.enumerated() {
    let key = String(format: "%.4f,%.4f", item.1.coordinate.latitude, item.1.coordinate.longitude)
    if let cached = cache[key] {
        result[item.0] = cached
        continue
    }
    var attempt = 0
    while true {
        let done = DispatchSemaphore(value: 0)
        var found: [String: Any]?
        var failure: Error?
        geocoder.reverseGeocodeLocation(item.1, preferredLocale: locale) { marks, error in
            if let mark = marks?.first { found = fields(mark) }
            failure = error
            done.signal()
        }
        while done.wait(timeout: .now()) == .timedOut {
            RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        }
        if let found = found {
            cache[key] = found
            result[item.0] = found
            break
        }
        attempt += 1
        let code = (failure as NSError?)?.code ?? -1
        if code == CLError.geocodeFoundNoResult.rawValue || attempt >= 5 {
            FileHandle.standardError.write("\(item.0) 失败: \(String(describing: failure))\n".data(using: .utf8)!)
            break
        }
        Thread.sleep(forTimeInterval: 30)
    }
    print("\(index + 1)/\(coordinates.count) \(result[item.0]?["city"] ?? "-") \(result[item.0]?["name"] ?? "")")
    Thread.sleep(forTimeInterval: 1.3)
}

let data = try JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted, .sortedKeys])
try data.write(to: URL(fileURLWithPath: args[2]))
print("地址数量: \(result.count)")
