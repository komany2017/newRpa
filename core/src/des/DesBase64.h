#pragma once

class DesBase64
{
public:
    DesBase64();
    virtual ~DesBase64();
    
    static int BASE64_Encode(const unsigned char* inputBuffer, int inputCount,
                             char* outputBuffer);
                             
    static int BASE64_Decode(const char* inputBuffer, int inputCount,
                             char* outputBuffer);
                             
};

