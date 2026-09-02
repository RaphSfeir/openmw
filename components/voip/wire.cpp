#include "wire.hpp"

namespace Voip
{
    bool readHeader(const unsigned char* data, std::size_t size, Header& out)
    {
        if (data == nullptr || size < sHeaderBytes)
            return false;

        out.mVersion = data[0];
        out.mFlags = data[1];
        out.mSeq = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(data[2]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[3]) << 8));
        out.mSpeaker = static_cast<std::uint32_t>(data[4]) | (static_cast<std::uint32_t>(data[5]) << 8)
            | (static_cast<std::uint32_t>(data[6]) << 16) | (static_cast<std::uint32_t>(data[7]) << 24);
        return true;
    }

    void writeHeader(unsigned char* data, const Header& header)
    {
        data[0] = header.mVersion;
        data[1] = header.mFlags;
        data[2] = static_cast<unsigned char>(header.mSeq & 0xff);
        data[3] = static_cast<unsigned char>((header.mSeq >> 8) & 0xff);
        writeSpeaker(data, header.mSpeaker);
    }

    void writeSpeaker(unsigned char* data, std::uint32_t speaker)
    {
        data[4] = static_cast<unsigned char>(speaker & 0xff);
        data[5] = static_cast<unsigned char>((speaker >> 8) & 0xff);
        data[6] = static_cast<unsigned char>((speaker >> 16) & 0xff);
        data[7] = static_cast<unsigned char>((speaker >> 24) & 0xff);
    }
}
