import Foundation

/// HEVC's levels and tiers (ITU-T H.265, Annex A): the picture size, frame
/// rate and data rate a player that declares a level promises to decode.
///
/// Export's HEVC rates come from here (`HEVCLevel`), and every HEVC export is
/// labelled with the lowest level and tier that holds it, as cameras label
/// theirs. VideoToolbox, which writes the file, only ever labels Main tier,
/// so it climbs levels with the data rate: a 4K 30 fps clip at 100 Mbit/s
/// comes out labelled level 6.1, an 8K level, and above 240 Mbit/s level 7,
/// which few players know. The same clip from a Nikon or a Lumix says High
/// tier, level 5.2.
enum HEVCLevels {

    /// One level: the largest picture, in luma samples, the luma samples a
    /// second, and the data rate Main and Main 10 may have at each tier, in
    /// Mbit/s (Tables A.8 and A.9; below level 4 there is no High tier).
    struct Limits: Sendable {
        let name: String
        /// general_level_idc, 30 times the level.
        let idc: UInt8
        let picture: Int
        let samples: Double
        let main: Double
        let high: Double?
        /// The most any tier of the level allows.
        var top: Double { high ?? main }
    }

    static let table: [Limits] = [
        Limits(name: "1", idc: 30, picture: 36_864, samples: 552_960, main: 0.128, high: nil),
        Limits(name: "2", idc: 60, picture: 122_880, samples: 3_686_400, main: 1.5, high: nil),
        Limits(name: "2.1", idc: 63, picture: 245_760, samples: 7_372_800, main: 3, high: nil),
        Limits(name: "3", idc: 90, picture: 552_960, samples: 16_588_800, main: 6, high: nil),
        Limits(name: "3.1", idc: 93, picture: 983_040, samples: 33_177_600, main: 10, high: nil),
        Limits(name: "4", idc: 120, picture: 2_228_224, samples: 66_846_720, main: 12, high: 30),
        Limits(name: "4.1", idc: 123, picture: 2_228_224, samples: 133_693_440, main: 20, high: 50),
        Limits(name: "5", idc: 150, picture: 8_912_896, samples: 267_386_880, main: 25, high: 100),
        Limits(name: "5.1", idc: 153, picture: 8_912_896, samples: 534_773_760, main: 40, high: 160),
        Limits(name: "5.2", idc: 156, picture: 8_912_896, samples: 1_069_547_520, main: 60, high: 240),
        Limits(name: "6", idc: 180, picture: 35_651_584, samples: 1_069_547_520, main: 60, high: 240),
        Limits(name: "6.1", idc: 183, picture: 35_651_584, samples: 2_139_095_040, main: 120, high: 480),
        Limits(name: "6.2", idc: 186, picture: 35_651_584, samples: 4_278_190_080, main: 240, high: 800),
    ]

    /// The first level that holds pictures `width` x `height` at `frameRate`:
    /// their size, their longer side (no more than the square root of 8 times
    /// the level's size) and their luma samples a second. 4K needs level 5 up
    /// to 30 fps, 5.1 up to 60 and 5.2 up to 120; HD 4, 4.1 and 5.
    static func needed(width: Int, height: Int, frameRate: Double) -> Int {
        let pixels = Double(width * height)
        let side = Double(max(width, height))
        let samples = pixels * max(frameRate, 1)
        return table.firstIndex { pixels <= Double($0.picture) && side * side <= 8 * Double($0.picture)
            && samples <= $0.samples } ?? table.count - 1
    }

    /// The limits from the clip's own level upward, each larger than the one
    /// before: the most its level allows, the next level's, the one after.
    /// 4K at 30 fps: 100, 160 and 240 Mbit/s; at 60, 160, 240 and 480.
    static func steps(width: Int, height: Int, frameRate: Double) -> [Double] {
        var steps: [Double] = []
        for limits in table[needed(width: width, height: height, frameRate: frameRate)...]
        where steps.last.map({ limits.top > $0 }) ?? true {
            steps.append(limits.top)
        }
        while steps.count < 3 { steps.append(steps[steps.count - 1]) }
        return Array(steps.prefix(3))
    }

    /// A level and tier, as a file declares them.
    struct Label: Equatable, Sendable, CustomStringConvertible {
        var high: Bool
        var idc: UInt8
        var description: String {
            let name = table.first { $0.idc == idc }?.name ?? String(format: "%.1f", Double(idc) / 30)
            return "\(high ? "High" : "Main") tier, level \(name)"
        }
    }

    /// The lowest level, and in it the lower tier, that holds the clip at
    /// `megabits`: Main tier while the rate is within it, High tier above.
    static func label(width: Int, height: Int, frameRate: Double, megabits: Double) -> Label {
        let rate = megabits - 1e-9
        for limits in table[needed(width: width, height: height, frameRate: frameRate)...] {
            if rate <= limits.main { return Label(high: false, idc: limits.idc) }
            if let high = limits.high, rate <= high { return Label(high: true, idc: limits.idc) }
        }
        return Label(high: true, idc: table[table.count - 1].idc)
    }

    // MARK: - The label in a file

    enum LabelError: Error { case unreadable(String) }

    /// Where the label is: the hvcC record of the movie's first HEVC sample
    /// description, at `offset` in the file.
    private struct Record {
        var offset: UInt64
        var bytes: [UInt8]
    }

    /// The label an HEVC movie declares, from its hvcC record; nil if it has
    /// no HEVC video.
    static func label(of url: URL) throws -> Label? {
        guard let record = try record(in: url) else { return nil }
        return Label(high: record.bytes[1] & 0x20 != 0, idc: record.bytes[12])
    }

    /// Labels an HEVC movie `label` in place: the hvcC record's level and
    /// tier, and the same in its VPS and SPS, where players read them. Nothing
    /// else changes, so the pictures decode as they did. Returns false, the
    /// file untouched, if the movie is not one it knows how to relabel: no
    /// hvcC, parameter sets that disagree with it, or sub-layers.
    @discardableResult
    static func relabel(_ url: URL, as label: Label) throws -> Bool {
        guard var record = try record(in: url) else { return false }
        var bytes = record.bytes
        guard bytes.count > 23 else { return false }
        let tierByte = bytes[1], level = bytes[12]
        var positions: [Int] = []   // record positions of each set's tier byte and level byte
        var at = 23
        for _ in 0..<Int(bytes[22]) {
            guard at + 3 <= bytes.count else { return false }
            let type = bytes[at] & 0x3f
            let count = Int(bytes[at + 1]) << 8 | Int(bytes[at + 2])
            at += 3
            for _ in 0..<count {
                guard at + 2 <= bytes.count else { return false }
                let length = Int(bytes[at]) << 8 | Int(bytes[at + 1])
                let start = at + 2, end = start + length
                guard end <= bytes.count else { return false }
                at = end
                guard type == 32 || type == 33 else { continue }
                // the payload past the two-byte NAL header, its emulation
                // prevention bytes (00 00 03) left out, mapped to the record
                var payload: [UInt8] = [], place: [Int] = []
                var zeros = 0
                for i in (start + 2)..<end {
                    if zeros >= 2 && bytes[i] == 3 { zeros = 0; continue }
                    payload.append(bytes[i]); place.append(i)
                    zeros = bytes[i] == 0 ? zeros + 1 : 0
                }
                // VPS: id, flags and layers in two bytes, 0xffff, then
                // profile_tier_level; SPS: id and sub-layers in one byte, then it.
                let ptl = type == 32 ? 4 : 1
                guard payload.count > ptl + 11 else { return false }
                let subLayers = type == 32 ? (payload[1] >> 1) & 7 : (payload[0] >> 1) & 7
                guard subLayers == 0, payload[ptl] == tierByte, payload[ptl + 11] == level else { return false }
                positions += [place[ptl], place[ptl + 11]]
            }
        }
        guard positions.count >= 4 else { return false }
        let newTier = label.high ? tierByte | 0x20 : tierByte & ~0x20
        bytes[1] = newTier
        bytes[12] = label.idc
        for (n, position) in positions.enumerated() { bytes[position] = n % 2 == 0 ? newTier : label.idc }
        guard bytes != record.bytes else { return true }
        record.bytes = bytes
        let handle = try FileHandle(forUpdating: url)
        defer { try? handle.close() }
        try handle.seek(toOffset: record.offset)
        try handle.write(contentsOf: Data(bytes))
        try handle.synchronize()
        return true
    }

    /// The movie's hvcC record: moov, trak, mdia, minf, stbl, stsd, hvc1 or
    /// hev1, hvcC. The media data is skipped, never read.
    private static func record(in url: URL) throws -> Record? {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        let size = try handle.seekToEnd()
        var at: UInt64 = 0
        while at + 8 <= size {
            try handle.seek(toOffset: at)
            guard let head = try handle.read(upToCount: 16), head.count >= 8 else { break }
            let h = [UInt8](head)
            var length = UInt64(h[0]) << 24 | UInt64(h[1]) << 16 | UInt64(h[2]) << 8 | UInt64(h[3])
            var header: UInt64 = 8
            if length == 1 {
                guard h.count >= 16 else { break }
                length = h[8..<16].reduce(0) { $0 << 8 | UInt64($1) }
                header = 16
            } else if length == 0 {
                length = size - at
            }
            guard length >= header, at + length <= size else {
                throw LabelError.unreadable("a box runs past the end of the file")
            }
            if String(bytes: h[4..<8], encoding: .ascii) == "moov" {
                try handle.seek(toOffset: at + header)
                let moov = [UInt8](try handle.read(upToCount: Int(length - header)) ?? Data())
                guard let found = find(moov, from: 0, to: moov.count, path: ["trak", "mdia", "minf", "stbl",
                                                                              "stsd", "hvc1", "hvcC"]) else { return nil }
                return Record(offset: at + header + UInt64(found.lowerBound), bytes: Array(moov[found]))
            }
            at += length
        }
        return nil
    }

    /// The payload of the box at the end of `path`, under the boxes from
    /// `from` to `to` in `data`. A sample description's entries start 8
    /// bytes in, a visual sample entry's boxes 78; "hvc1" matches hev1 too.
    private static func find(_ data: [UInt8], from: Int, to: Int, path: [String]) -> Range<Int>? {
        var at = from
        while at + 8 <= to {
            let length = Int(data[at]) << 24 | Int(data[at + 1]) << 16 | Int(data[at + 2]) << 8 | Int(data[at + 3])
            guard length >= 8, at + length <= to else { return nil }
            let type = String(bytes: data[(at + 4)..<(at + 8)], encoding: .ascii) ?? ""
            if type == path[0] || (path[0] == "hvc1" && type == "hev1") {
                if path.count == 1 { return (at + 8)..<(at + length) }
                let inner = at + 8 + (type == "stsd" ? 8 : type == "hvc1" || type == "hev1" ? 78 : 0)
                if let found = find(data, from: inner, to: at + length, path: Array(path.dropFirst())) {
                    return found
                }
            }
            at += length
        }
        return nil
    }
}
