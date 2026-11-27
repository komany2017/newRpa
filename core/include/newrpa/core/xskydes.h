#pragma once
// ============================================================================
// xskydes — 新网银行/兴业系 DES 加解密（公共 crypto）
//
// 来源: core/src/des/xskydes.cpp（原 DES.cpp，ECB/CBC + ISO_1/ISO_2/PKCS7 填充）
//
// 用法:
//   char out[4096] = {0};
//   DES::DesDecrypt(hexOrCipherText, out);          // 16 进制密文，自动补位
//   DES::DesDecrypt2(cipher, len, out);             // 指定长度
// ============================================================================
#ifndef __XSKY_DES_H__
#define __XSKY_DES_H__

namespace DES {

int DesEncrypt(char* sIn, int inlen, char* sOut, int* outlen);
int DesDecrypt(char* sIn, char* sOut);
int DesDecrypt2(char* sIn, int inlen, char* sOut);

} // namespace DES

#endif // __XSKY_DES_H__
