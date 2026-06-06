#include "SeratoCueWriter.h"

#include "SeratoMarkers2.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include <taglib/aifffile.h>
#include <taglib/flacfile.h>
#include <taglib/generalencapsulatedobjectframe.h>
#include <taglib/id3v2tag.h>
#include <taglib/mp4atom.h>
#include <taglib/mp4file.h>
#include <taglib/mp4item.h>
#include <taglib/mp4tag.h>
#include <taglib/mpegfile.h>
#include <taglib/vorbisfile.h>
#include <taglib/wavfile.h>
#include <taglib/tbytevector.h>
#include <taglib/tbytevectorlist.h>
#include <taglib/tstringlist.h>
#include <taglib/xiphcomment.h>

namespace fs = std::filesystem;

namespace {

std::string lowerExtension(const fs::path& file)
{
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

bool isMp3(const fs::path& file)
{
    return lowerExtension(file) == ".mp3";
}

bool isWav(const fs::path& file)
{
    return lowerExtension(file) == ".wav";
}

bool isAiff(const fs::path& file)
{
    std::string ext = lowerExtension(file);
    return ext == ".aif" || ext == ".aiff";
}

bool isFlac(const fs::path& file)
{
    return lowerExtension(file) == ".flac";
}

bool isOgg(const fs::path& file)
{
    return lowerExtension(file) == ".ogg";
}

bool isMp4(const fs::path& file)
{
    std::string ext = lowerExtension(file);
    return ext == ".m4a" || ext == ".mp4" || ext == ".aac" || ext == ".alac";
}

bool isSupportedSeratoContainer(const fs::path& file)
{
    return isMp3(file) || isWav(file) || isAiff(file) ||
           isFlac(file) || isOgg(file) || isMp4(file);
}

TagLib::ByteVector toByteVector(const std::vector<std::uint8_t>& bytes)
{
    return TagLib::ByteVector(reinterpret_cast<const char*>(bytes.data()),
                              (unsigned int)bytes.size());
}

TagLib::String toLatin1String(const std::vector<std::uint8_t>& bytes)
{
    return TagLib::String(std::string(reinterpret_cast<const char*>(bytes.data()),
                                      bytes.size()),
                          TagLib::String::Latin1);
}

std::vector<std::uint8_t> fromTagLibString(const TagLib::String& value)
{
    std::string text = value.to8Bit(true);
    return {text.begin(), text.end()};
}

std::vector<std::uint8_t> fromByteVector(const TagLib::ByteVector& value)
{
    std::vector<std::uint8_t> out((size_t)value.size());
    for (int i = 0; i < value.size(); ++i) {
        out[(size_t)i] = (std::uint8_t)value[i];
    }
    return out;
}

std::vector<SeratoCue> readId3Markers2(TagLib::ID3v2::Tag* tag)
{
    if (tag == nullptr) {
        return {};
    }
    auto frames = tag->frameList("GEOB");
    for (auto* frame : frames) {
        auto* geob = dynamic_cast<TagLib::ID3v2::GeneralEncapsulatedObjectFrame*>(frame);
        if (geob != nullptr && geob->description() == "Serato Markers2") {
            return decodeSeratoMarkers2(fromByteVector(geob->object()));
        }
    }
    return {};
}

bool writeId3Markers2(TagLib::ID3v2::Tag* tag,
                      const std::vector<SeratoCue>& cues,
                      bool overwriteExistingCues,
                      std::string& error)
{
    if (tag == nullptr) {
        error = "Could not create ID3v2 tag";
        return false;
    }

    auto frames = tag->frameList("GEOB");
    for (auto* frame : frames) {
        auto* geob = dynamic_cast<TagLib::ID3v2::GeneralEncapsulatedObjectFrame*>(frame);
        if (geob != nullptr && geob->description() == "Serato Markers2") {
            if (!overwriteExistingCues) {
                return true;
            }
            tag->removeFrame(frame, true);
        }
    }

    if (cues.empty()) {
        return true;
    }

    auto* markers = new TagLib::ID3v2::GeneralEncapsulatedObjectFrame;
    markers->setTextEncoding(TagLib::String::Latin1);
    markers->setMimeType("application/octet-stream");
    markers->setFileName("");
    markers->setDescription("Serato Markers2");
    markers->setObject(toByteVector(encodeSeratoMarkers2(cues)));
    tag->addFrame(markers);
    return true;
}

std::vector<std::uint8_t> readXiphField(TagLib::Ogg::XiphComment* tag,
                                        const TagLib::StringList& keys)
{
    if (tag == nullptr) {
        return {};
    }
    const auto& fields = tag->fieldListMap();
    for (const auto& key : keys) {
        if (!fields.contains(key)) {
            continue;
        }
        const auto values = fields[key];
        if (!values.isEmpty()) {
            return fromTagLibString(values.front());
        }
    }
    return {};
}

bool xiphHasAny(TagLib::Ogg::XiphComment* tag, const TagLib::StringList& keys)
{
    if (tag == nullptr) {
        return false;
    }
    const auto& fields = tag->fieldListMap();
    for (const auto& key : keys) {
        if (fields.contains(key)) {
            return true;
        }
    }
    return false;
}

void replaceXiphField(TagLib::Ogg::XiphComment* tag,
                      const TagLib::StringList& keys,
                      const TagLib::String& writeKey,
                      const std::vector<std::uint8_t>& bytes)
{
    for (const auto& key : keys) {
        tag->removeFields(key);
    }
    tag->addField(writeKey, toLatin1String(bytes), true);
}

void removeXiphFields(TagLib::Ogg::XiphComment* tag,
                      const TagLib::StringList& keys)
{
    if (tag == nullptr) {
        return;
    }
    for (const auto& key : keys) {
        tag->removeFields(key);
    }
}

bool createBackup(const fs::path& file, std::string& error)
{
    fs::path backup = file;
    backup += ".autocue.bak";
    std::error_code ec;
    if (!fs::exists(backup, ec)) {
        fs::copy_file(file, backup, fs::copy_options::none, ec);
        if (ec) {
            error = "Could not create backup: " + ec.message();
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<SeratoCue> SeratoCueWriter::readCues(const fs::path& file,
                                                 std::string& error) const
{
    if (isMp4(file)) {
        TagLib::MP4::File mp4(file.c_str(), false);
        if (!mp4.isValid() || mp4.tag() == nullptr) {
            error = "Could not open MP4/M4A with TagLib";
            return {};
        }
        const TagLib::String markers2_key("----:com.serato.dj:markersv2");
        TagLib::MP4::Tag* tag = mp4.tag();
        if (!tag->contains(markers2_key)) {
            return {};
        }
        auto item = tag->item(markers2_key);
        std::vector<std::uint8_t> bytes;
        auto strings = item.toStringList();
        if (!strings.isEmpty()) {
            bytes = fromTagLibString(strings.front());
        } else {
            auto byte_lists = item.toByteVectorList();
            if (!byte_lists.isEmpty()) {
                bytes = fromByteVector(byte_lists.front());
            }
        }
        return decodeSeratoMarkers2Mp4(bytes);
    }

    if (isMp3(file)) {
        TagLib::MPEG::File mp3(file.c_str(), false);
        if (!mp3.isValid()) {
            error = "Could not open MP3 with TagLib";
            return {};
        }
        return readId3Markers2(mp3.ID3v2Tag(false));
    }

    if (isWav(file)) {
        TagLib::RIFF::WAV::File wav(file.c_str(), false);
        if (!wav.isValid()) {
            error = "Could not open WAV with TagLib";
            return {};
        }
        return readId3Markers2(wav.ID3v2Tag());
    }

    if (isAiff(file)) {
        TagLib::RIFF::AIFF::File aiff(file.c_str(), false);
        if (!aiff.isValid()) {
            error = "Could not open AIFF with TagLib";
            return {};
        }
        return readId3Markers2(aiff.tag());
    }

    if (isFlac(file)) {
        TagLib::FLAC::File flac(file.c_str(), false);
        if (!flac.isValid()) {
            error = "Could not open FLAC with TagLib";
            return {};
        }
        TagLib::StringList keys;
        keys.append("SERATO_MARKERS_V2");
        keys.append("serato_markers_v2");
        return decodeSeratoMarkers2Mp4(readXiphField(flac.xiphComment(false), keys));
    }

    if (isOgg(file)) {
        TagLib::Ogg::Vorbis::File ogg(file.c_str(), false);
        if (!ogg.isValid()) {
            error = "Could not open OGG with TagLib";
            return {};
        }
        TagLib::StringList keys;
        keys.append("SERATO_MARKERS2");
        keys.append("serato_markers2");
        return decodeSeratoMarkers2Ogg(readXiphField(ogg.tag(), keys));
    }

    error = "Serato read skipped: unsupported container";
    return {};
}

bool SeratoCueWriter::writeCues(const fs::path& file,
                                const std::vector<SeratoCue>& cues,
                                std::string& error)
{
    return writeCues(file, cues, error, true, true);
}

bool SeratoCueWriter::writeCues(const fs::path& file,
                                const std::vector<SeratoCue>& cues,
                                std::string& error,
                                bool backupBeforeWrite,
                                bool overwriteExistingCues)
{
    if (!isSupportedSeratoContainer(file)) {
        error = "Serato write skipped: unsupported container";
        return false;
    }

    if (backupBeforeWrite && !createBackup(file, error)) {
        return false;
    }

    if (isMp4(file)) {
        TagLib::MP4::File mp4(file.c_str(), false);
        if (!mp4.isValid()) {
            error = "Could not open MP4/M4A with TagLib";
            return false;
        }

        TagLib::MP4::Tag* tag = mp4.tag();
        if (tag == nullptr) {
            error = "Could not create MP4 tag";
            return false;
        }

        const TagLib::String markers2_key("----:com.serato.dj:markersv2");
        const TagLib::String markers_key("----:com.serato.dj:markers");
        if (!overwriteExistingCues && tag->contains(markers2_key)) {
            return true;
        }

        tag->removeItem(markers2_key);
        tag->removeItem(markers_key);
        if (cues.empty()) {
            if (!mp4.save()) {
                error = "TagLib failed to save MP4/M4A tag";
                return false;
            }
            return true;
        }

        TagLib::StringList markers2_data;
        markers2_data.append(toLatin1String(encodeSeratoMarkers2Mp4(cues)));
        TagLib::MP4::Item markers2(markers2_data);
        markers2.setAtomDataType(TagLib::MP4::TypeUTF8);
        tag->setItem(markers2_key, markers2);

        TagLib::StringList markers_data;
        markers_data.append(toLatin1String(encodeSeratoMarkersLegacyMp4(cues)));
        TagLib::MP4::Item markers(markers_data);
        markers.setAtomDataType(TagLib::MP4::TypeUTF8);
        tag->setItem(markers_key, markers);

        if (!mp4.save()) {
            error = "TagLib failed to save MP4/M4A tag";
            return false;
        }
        return true;
    }

    if (isMp3(file)) {
        TagLib::MPEG::File mp3(file.c_str(), false);
        if (!mp3.isValid()) {
            error = "Could not open MP3 with TagLib";
            return false;
        }
        if (!writeId3Markers2(mp3.ID3v2Tag(true), cues, overwriteExistingCues, error)) {
            return false;
        }
        if (!mp3.save(TagLib::MPEG::File::ID3v2,
                      TagLib::MPEG::File::StripOthers,
                      TagLib::ID3v2::v4)) {
            error = "TagLib failed to save MP3 ID3v2 tag";
            return false;
        }
        return true;
    }

    if (isWav(file)) {
        TagLib::RIFF::WAV::File wav(file.c_str(), false);
        if (!wav.isValid()) {
            error = "Could not open WAV with TagLib";
            return false;
        }
        if (!writeId3Markers2(wav.ID3v2Tag(), cues, overwriteExistingCues, error)) {
            return false;
        }
        if (!wav.save(TagLib::RIFF::WAV::File::ID3v2,
                      TagLib::RIFF::WAV::File::StripOthers,
                      TagLib::ID3v2::v4)) {
            error = "TagLib failed to save WAV ID3v2 tag";
            return false;
        }
        return true;
    }

    if (isAiff(file)) {
        TagLib::RIFF::AIFF::File aiff(file.c_str(), false);
        if (!aiff.isValid()) {
            error = "Could not open AIFF with TagLib";
            return false;
        }
        if (!writeId3Markers2(aiff.tag(), cues, overwriteExistingCues, error)) {
            return false;
        }
        if (!aiff.save(TagLib::ID3v2::v4)) {
            error = "TagLib failed to save AIFF ID3v2 tag";
            return false;
        }
        return true;
    }

    if (isFlac(file)) {
        TagLib::FLAC::File flac(file.c_str(), false);
        if (!flac.isValid()) {
            error = "Could not open FLAC with TagLib";
            return false;
        }
        TagLib::Ogg::XiphComment* tag = flac.xiphComment(true);
        TagLib::StringList keys;
        keys.append("SERATO_MARKERS_V2");
        keys.append("serato_markers_v2");
        if (!overwriteExistingCues && xiphHasAny(tag, keys)) {
            return true;
        }
        if (cues.empty()) {
            removeXiphFields(tag, keys);
            if (!flac.save()) {
                error = "TagLib failed to save FLAC Vorbis comment";
                return false;
            }
            return true;
        }
        replaceXiphField(tag, keys, "SERATO_MARKERS_V2", encodeSeratoMarkers2Mp4(cues));
        if (!flac.save()) {
            error = "TagLib failed to save FLAC Vorbis comment";
            return false;
        }
        return true;
    }

    if (isOgg(file)) {
        TagLib::Ogg::Vorbis::File ogg(file.c_str(), false);
        if (!ogg.isValid()) {
            error = "Could not open OGG with TagLib";
            return false;
        }
        TagLib::Ogg::XiphComment* tag = ogg.tag();
        TagLib::StringList keys;
        keys.append("SERATO_MARKERS2");
        keys.append("serato_markers2");
        if (!overwriteExistingCues && xiphHasAny(tag, keys)) {
            return true;
        }
        if (cues.empty()) {
            removeXiphFields(tag, keys);
            if (!ogg.save()) {
                error = "TagLib failed to save OGG Vorbis comment";
                return false;
            }
            return true;
        }
        replaceXiphField(tag, keys, "serato_markers2", encodeSeratoMarkers2Ogg(cues));
        if (!ogg.save()) {
            error = "TagLib failed to save OGG Vorbis comment";
            return false;
        }
        return true;
    }

    error = "Serato write skipped: unsupported container";
    return false;
}
