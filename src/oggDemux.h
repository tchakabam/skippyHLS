//
// Created by Alexander Lenhardt on 11/16/15.
//

#ifndef OGGOPUSDEC_OGGOPUSDEC_H_H
#define OGGOPUSDEC_OGGOPUSDEC_H_H

#ifdef  __cplusplus
#include <istream>
#include <vector>
#include <memory>
#include <cstdint>

class OggDecoder
{
public:
    struct Impl;
    OggDecoder();
    ~OggDecoder();
    void read(std::istream& stream);
    void setLastSeekingPosition(int64_t lastSeekingPoistion);
    void flush();
    std::shared_ptr<Impl> priv() { return m_priv; } // ouch
private:
    std::vector<float> decoded;
    std::shared_ptr<Impl> m_priv;
};
#endif

typedef void* COggDecoder;
typedef struct {
    unsigned char* payload;
    int64_t granulepos;
    size_t len;
} OpusPacket;

#ifdef  __cplusplus
extern "C" {
#endif
    COggDecoder createOggDecoder        ();
    void        destroyOggDecoder       (COggDecoder);
    void        onDataReceived          (COggDecoder oggDec, char* buffer, size_t bufferLen);
    int         readPage                (COggDecoder oggDec);
    int         readPacket              (COggDecoder oggDec, OpusPacket* pkt);
    void        setLastSeekingPosition  (COggDecoder oggDec, int64_t lastSeekingPoistion);
    void        flushDecoder            (COggDecoder oggDec);
#ifdef  __cplusplus
}
#endif
#endif //OGGOPUSDEC_OGGOPUSDEC_H_H
