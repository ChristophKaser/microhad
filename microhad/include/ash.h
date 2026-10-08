#ifndef MICROHAD_ZIGBEE_ASH_H
#define MICROHAD_ZIGBEE_ASH_H

#include <string>
#include <vector>

class AshTransport {
public:
    AshTransport();
    static unsigned short crc16(const std::vector<unsigned char>& data);
    static std::vector<unsigned char> encodeData(unsigned char control,
                                                  const std::vector<unsigned char>& payload);
    void feed(const unsigned char* bytes, unsigned long length);
    bool popFrame(std::vector<unsigned char>* frame);
    void reset();

private:
    std::vector<unsigned char> input_;
    std::vector<std::vector<unsigned char> > frames_;
    void parse();
};

#endif
