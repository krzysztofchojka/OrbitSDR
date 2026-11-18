#include "base64.h"

// Base64 char table - used internally for encoding
static unsigned char b64Chr[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint32_t b64Int(uint32_t ch)
{
    // ASCII to base64_int
    // 65-90  Upper Case  >>  0-25
    // 97-122 Lower Case  >>  26-51
    // 48-57  Numbers     >>  52-61
    // 43     Plus (+)    >>  62
    // 47     Slash (/)   >>  63
    // 61     Equal (=)   >>  64~
    if (ch == 43)
        return 62;
    if (ch == 47)
        return 63;
    if (ch == 61)
        return 64;
    if ((ch > 47) && (ch < 58))
        return ch + 4;
    if ((ch > 64) && (ch < 91))
        return ch - 'A';
    if ((ch > 96) && (ch < 123))
        return (ch - 'a') + 26;

    return 0;
}

uint32_t Tetra::b64eSize(uint32_t inSize)
{
    // size equals 4 * floor((1 / 3) * (inSize + 2));
    uint32_t len = 0;

    for (uint32_t idx = 0; idx < inSize; idx++)
    {
        if (idx % 3 == 0)
        {
            len++;
        }
    }
    return 4 * len;
}

uint32_t Tetra::b64dSize(uint32_t inSize)
{
    return (3 * inSize) / 4;
}

unsigned int Tetra::b64Encode(const unsigned char * in, uint32_t inLen, unsigned char * out)
{
    uint32_t data[3];
    uint32_t byt = 0;                                                           // can be 0, 1 or 2 since 3 bytes in encode
    uint32_t pos = 0;                                                           // current output character position

    for (uint32_t idx = 0; idx < inLen; idx++)
    {
        data[byt++] = *(in + idx);

        if (byt == 3)                                                           // process data
        {
            out[pos + 0] = b64Chr[ (data[0] & 0xFF) >> 2 ];
            out[pos + 1] = b64Chr[((data[0] & 0x03) << 4) + ((data[1] & 0xF0) >> 4)];
            out[pos + 2] = b64Chr[((data[1] & 0x0F) << 2) + ((data[2] & 0xC0) >> 6)];
            out[pos + 3] = b64Chr[  data[2] & 0x3F];
            byt  = 0;                                                           // reset byte position
            pos += 4;
        }
    }

    if (byt)                                                                    // there is remaining data to process
    {
        if (byt == 1)
        {
            data[1] = 0;
        }

        out[pos + 0] = b64Chr[ (data[0] & 0xFF) >> 2];
        out[pos + 1] = b64Chr[((data[0] & 0x03) << 4) + ((data[1] & 0xF0) >> 4)];

        if (byt == 2)
        {
            out[pos + 2] = b64Chr[((data[1] & 0x0F) << 2)];
        }
        else                                                                    // add padding
        {
            out[pos + 2] = '=';
        }

        out[pos + 3] = '=';                                                     // padding
        pos += 4;
    }

    out[pos] = '\0';                                                            // final character

    return pos;                                                                 // encoded characters count (including final \0)
}

unsigned int Tetra::b64Decode(const unsigned char * in, unsigned int inLen, unsigned char * out)
{
    uint32_t byt = 0;                                                           // can be 0, 1, 2 or 3 since 4 bytes in decode
    uint32_t pos = 0;                                                           // current output character position
    uint32_t data[4];

    for (uint32_t idx = 0; idx < inLen; idx++)
    {
        data[byt++] = b64Int(*(in + idx));

        if (byt == 4)
        {
            out[pos + 0] = ((data[0] & 0xFF) << 2) + ((data[1] & 0x30) >> 4);

            if (data[2] != 64)
            {
                out[pos + 1] = ((data[1] & 0x0F) << 4) + ((data[2] & 0x3C) >> 2);

                if (data[3] != 64)
                {
                    out[pos + 2] = ((data[2] & 0x03) << 6) + data[3];
                    pos += 3;
                }
                else
                {
                    pos += 2;
                }
            }
            else
            {
                pos += 1;
            }
            byt = 0;
        }
    }

    return pos;                                                                 // decoded characters count
}
