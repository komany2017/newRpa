// DES.cpp: implementation of the CDES class.
//
//////////////////////////////////////////////////////////////////////
#include "xskydes.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <memory.h>


namespace DES
{

//DES算法的模式
enum
{
    ECB = 0,	//ECB模式
    CBC				//CBC模式
};

//加密解密
enum
{
    XENCRYPT = 0,	//加密
    XDECRYPT		//解密
};

//Pad填充的模式
enum
{
    PAD_ISO_1 = 0,	//ISO_1填充：数据长度不足8比特的倍数，以0x00补足，如果为8比特的倍数，补8个0x00
    PAD_ISO_2,		//ISO_2填充：数据长度不足8比特的倍数，以0x80,0x00..补足，如果为8比特的倍数，补0x80,0x00..0x00
    PAD_PKCS_7		//PKCS7填充：数据长度除8余数为n,以(8-n)补足为8的倍数
};

typedef unsigned char(*PSubKey)[16][48];

////////////////////////////////////////////////////////////////////////
// initial permutation IP
const unsigned char IP_Table[64] =
{
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7
};

// final permutation IP^-1
const unsigned char IPR_Table[64] =
{
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25
};

// expansion operation matrix
const unsigned char E_Table[48] =
{
    32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9,
    8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25,
    24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1
};

// 32-bit permutation function P used on the output of the S-boxes
const unsigned char P_Table[32] =
{
    16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
    2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25
};

// permuted choice table (key)
const unsigned char PC1_Table[56] =
{
    57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18,
    10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
    14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4
};

// permuted choice key (table)
const unsigned char PC2_Table[48] =
{
    14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10,
    23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32
};

// number left rotations of pc1
const unsigned char LOOP_Table[16] =
{
    1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1
};

// The (in)famous S-boxes
const unsigned char S_Box[8][4][16] =
{
    // S1
    {
        { 14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, },
        { 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8, },
        { 4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, },
        { 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13, },
    },
    
    // S2
    {
        { 15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, },
        { 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5, },
        { 0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, },
        { 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9, },
    },
    
    // S3
    {
        { 10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, },
        { 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1, },
        { 13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, },
        { 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12, },
    },
    
    // S4
    {
        { 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, },
        { 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9, },
        { 10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, },
        { 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14, },
    },
    
    // S5
    {
        { 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, },
        { 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6, },
        { 4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, },
        { 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3, },
    },
    
    // S6
    {
        { 12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, },
        { 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8, },
        { 9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, },
        { 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13, },
    },
    
    // S7
    {
        { 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, },
        { 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6, },
        { 1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, },
        { 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12, },
    },
    
    // S8
    {
        { 13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, },
        { 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2, },
        { 7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, },
        { 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11, },
    }
};


unsigned char des_iv[10] = "uiop6789";
unsigned char des_key[10] = "uiop6789";
int des_padmode = PAD_PKCS_7;
int des_enmode = CBC;

/*******************************************************************/
//函 数 名 称:	ByteToBit
//功 能 描 述：	把BYTE转化为Bit流
//参 数 说 明：	Out:	输出的Bit流[in][out]
//				In:		输入的BYTE流[in]
//				bits:	Bit流的长度[in]
//
//返回值 说明：	void
//作       者:	邹德强
//更 新 日 期：	2003.12.19
/*******************************************************************/
void ByteToBit(unsigned char* Out, unsigned char* In, int bits)
{
    for (int i = 0; i < bits; ++i)
    {
        Out[i] = (In[i >> 3] >> ((7 - i) & 7)) & 1;
    }
}

/*******************************************************************/
//函 数 名 称:	BitToByte
//功 能 描 述：	把Bit转化为Byte流
//参 数 说 明：	Out:	输出的BYTE流[in][out]
//				In:		输入的Bit流[in]
//				bits:	Bit流的长度[in]
//
//返回值 说明：	void
//作       者:	邹德强
//更 新 日 期：	2003.12.19
/*******************************************************************/
void BitToByte(unsigned char* Out, unsigned char* In, int bits)
{
    memset(Out, 0, bits >> 3);
    for (int i = 0; i < bits; ++i)
    {
        Out[i >> 3] |= In[i] << ((7 - i) & 7);
    }
}



/*******************************************************************/
//函 数 名 称:	RotateL
//功 能 描 述：	把BIT流按位向左迭代
//参 数 说 明：	In:		输入的Bit流[in]
//				len:	Bit流的长度[in]
//				loop:	向左迭代的长度
//
//返回值 说明：	void
//作       者:	邹德强
//更 新 日 期：	2003.12.19
/*******************************************************************/
void RotateL(unsigned char* In, int len, int loop)
{
    unsigned char Tmp[256];
    memcpy(Tmp, In, loop);
    memcpy(In, In + loop, len - loop);
    memcpy(In + len - loop, Tmp, loop);
}



/*******************************************************************/
//函 数 名 称:	Xor
//功 能 描 述：	把两个Bit流进行异或
//参 数 说 明：	InA:	输入的Bit流[in][out]
//InB:	输入的Bit流[in]
//loop:	Bit流的长度
//
//返回值 说明：	void
//作       者:	邹德强
//更 新 日 期：	2003.12.19
/*******************************************************************/
void Xor(unsigned char* InA, unsigned char* InB, int len)
{
    for (int i = 0; i < len; ++i)
    {
        InA[i] ^= InB[i];
    }
}


/*******************************************************************/
//函 数 名 称:	Transform
//功 能 描 述：	把两个Bit流按表进行位转化
//参 数 说 明：	Out:	输出的Bit流[out]
//				In:		输入的Bit流[in]
//				Table:	转化需要的表指针
//				len:	转化表的长度
//
//返回值 说明：	void
//作       者:	邹德强
//更 新 日 期：	2003.12.19
/*******************************************************************/
void Transform(unsigned char* Out, unsigned char* In,
               const unsigned char* Table, int len)
{
    unsigned char Tmp[256];
    for (int i = 0; i < len; ++i)
    {
        Tmp[i] = In[Table[i] - 1];
    }
    memcpy(Out, Tmp, len);
}


/*******************************************************************/
//函 数 名 称:	S_func
//功 能 描 述：	实现数据加密S BOX模块
//参 数 说 明：	Out:	输出的32Bit[out]
//				In:		输入的48Bit[in]
//
//返回值 说明：	void
//作       者:	邹德强
//更 新 日 期：	2003.12.19
/*******************************************************************/
void S_func(unsigned char Out[32], unsigned char In[48])
{
    for (unsigned char i = 0, j, k; i < 8; ++i, In += 6, Out += 4)
    {
        j = (In[0] << 1) + In[5];
        k = (In[1] << 3) + (In[2] << 2) + (In[3] << 1) + In[4];	//组织SID下标
        for (int l = 0; l < 4; ++l)								//把相应4bit赋值
        {
            Out[l] = (S_Box[i][j][k] >> (3 - l)) & 1;
        }
    }
}

/*******************************************************************/
//函 数 名 称:	F_func
//功 能 描 述：	实现数据加密到输出P
//参 数 说 明：	Out:	输出的32Bit[out]
//				In:		输入的48Bit[in]
//
//返回值 说明：	void
//作       者:	邹德强
//更 新 日 期：	2003.12.19
/*******************************************************************/
void F_func(unsigned char In[32], unsigned char Ki[48])
{
    unsigned char MR[48];
    Transform(MR, In, E_Table, 48);
    Xor(MR, Ki, 48);
    S_func(In, MR);
    Transform(In, In, P_Table, 32);
}

//计算并填充子密钥到SubKey数据中
void SetSubKey(PSubKey pSubKey, unsigned char Key[8])
{
    unsigned char K[64], *KL = &K[0], *KR = &K[28];
    ByteToBit(K, Key, 64);
    Transform(K, K, PC1_Table, 56);
    for (int i = 0; i < 16; ++i)
    {
        RotateL(KL, 28, LOOP_Table[i]);
        RotateL(KR, 28, LOOP_Table[i]);
        Transform((*pSubKey)[i], K, PC2_Table, 48);
    }
}

//DES单元运算
void DES(unsigned char Out[8], unsigned char In[8], const PSubKey pSubKey,
         unsigned char Type)
{
    unsigned char M[64], tmp[32], *Li = &M[0], *Ri = &M[32];
    ByteToBit(M, In, 64);
    Transform(M, M, IP_Table, 64);
    if (Type == XENCRYPT)
    {
        for (int i = 0; i < 16; ++i)
        {
            memcpy(tmp, Ri, 32);		//Ri[i-1] 保存
            F_func(Ri, (*pSubKey)[i]);	//Ri[i-1]经过转化和SBox输出为P
            Xor(Ri, Li, 32);			//Ri[i] = P XOR Li[i-1]
            memcpy(Li, tmp, 32);		//Li[i] = Ri[i-1]
        }
    }
    else
    {
        for (int i = 15; i >= 0; --i)
        {
            memcpy(tmp, Ri, 32);		//Ri[i-1] 保存
            F_func(Ri, (*pSubKey)[i]);	//Ri[i-1]经过转化和SBox输出为P
            Xor(Ri, Li, 32);			//Ri[i] = P XOR Li[i-1]
            memcpy(Li, tmp, 32);		//Li[i] = Ri[i-1]
        }
    }
    RotateL(M, 64, 32);					//Ri与Li换位重组M
    Transform(M, M, IPR_Table, 64);		//最后结果进行转化
    BitToByte(Out, M, 64);				//组织成字符
}


/*******************************************************************/
//函 数 名 称:	RunPad
//功 能 描 述：	根据协议对加密前的数据进行填充
//参 数 说 明：	bType	:类型：PAD类型
//			In		:数据串指针
//			Out		:填充输出串指针
//			datalen	:数据的长度
//			padlen	:(in,out)输出buffer的长度，填充后的长度
//
//返回值 说明：	bool	:是否填充成功
//作       者:	邹德强
//修 改 历 史：	2004.7.12日，更改内存写入位置错误
//
//更 新 日 期：	2003.12.19
/*******************************************************************/
int RunPad(int padmode, unsigned char* In, unsigned datalen,
           unsigned char* Out, unsigned* padlen)
{
    int res = (datalen & 0x00000007);
    *padlen = (datalen + 8 - res);
    if (*padlen < (datalen + 8 - res))
    {
        return 0;
    }
    memcpy(Out, In, datalen);
    if (padmode == PAD_ISO_1)
    {
        memset(Out + datalen, 0x00, 8 - res);
    }
    else if (padmode == PAD_ISO_2)     //错误
    {
        memset(Out + datalen, 0x80, 1);
        memset(Out + datalen, 0x00, 7 - res);
    }
    else if (padmode == PAD_PKCS_7)
    {
        memset(Out + datalen, 8 - res, 8 - res);
    }
    else
    {
        return 0;
    }
    return 1;
}

int ClearPad(int padmode, unsigned char* In, unsigned datalen,
             unsigned char* Out)
{
    if ((datalen & 0x00000007) != 0)
    {
        return 0;
    }
    else
    {
        memcpy(Out, In, datalen);
    }
    unsigned char res = Out[datalen - 1];
    //这个是按照服务器加密来的数据判断的，
    //理论上应该是7，因为要填充8字节就可以一个字节都不填
    int mexres = 8;//填充最大值
    if (res <= mexres)
    {
        int count;
        for (count = 0; count < mexres; count++)
        {
            if (Out[datalen - count - 1] != res)
            {
                break;
            }
        }
        //有填充*/
        if (count == res)
        {
            for (count = 0; count < res; count++)
            {
                Out[datalen - count - 1] = 0;
            }
        }
    }
    Out[datalen] = 0;
    return 1;
}


int string_to_value(char in[], char out[])
{
    int len_in = strlen(in);
    int len_out = len_in / 2;
    int i = 0;
    for (; i < len_out; i++)
    {
        char buf[5] = "";
        buf[0] = in[i * 2];
        buf[1] = in[i * 2 + 1];
        buf[2] = '\0';
        out[i] = (char)strtol(buf, NULL, 16);
    }
    out[i] = 0;
    return len_out;
}



/*******************************************************************/
//函 数 名 称:	RunDes
//功 能 描 述：	执行DES算法对文本加解密
//参 数 说 明：	bType	:类型：加密ENCRYPT，解密DECRYPT
//				bMode	:模式：ECB,CBC
//				In		:待加密串指针
//				Out		:待输出串指针
//				datalen	:待加密串的长度，同时Out的缓冲区大小应大于或者等于datalen
//				Key		:密钥(可为8位,16位,24位)支持3密钥
//				keylen	:密钥长度，多出24位部分将被自动裁减
//
//返回值 说明：	unsigned char	:是否加密成功
//作       者:	邹德强
//更 新 日 期：	2003.12.19
/*******************************************************************/
int DesEncrypt(char* sIn, int inlen, char* sOut, int* outlen)
{
    int bType = XENCRYPT;
    unsigned char* Key = des_key;
    int keylen = 8;
    unsigned char* In = (unsigned char*)sIn;
    unsigned char* Out = (unsigned char*)sOut;
    //判断输入合法性
    if (!(In && Out && Key && inlen && keylen >= 8))
    {
        return 0;
    }
    //只处理8的整数倍，不足长度自己填充
    //if (inlen & 0x00000007)
    {
        unsigned inlen_pad = 0;
        RunPad(des_padmode, In, inlen, In, &inlen_pad);
        inlen = inlen_pad;
    }
    *outlen = inlen;
    unsigned char m_SubKey[3][16][48];		//秘钥
    //构造并生成SubKeys
    unsigned char nKey = (keylen >> 3) > 3 ? 3 : (keylen >> 3);
    for (int i = 0; i < nKey; i++)
    {
        SetSubKey(&m_SubKey[i], &Key[i << 3]);
    }
    if (des_enmode == ECB)  	//ECB模式
    {
        if (nKey == 1)  	//单Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                DES(Out, In, &m_SubKey[0], bType);
            }
        }
        else if (nKey == 2)  	//3DES 2Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                DES(Out, In, &m_SubKey[0], bType);
                DES(Out, Out, &m_SubKey[1], !bType);
                DES(Out, Out, &m_SubKey[0], bType);
            }
        }
        else  		//3DES 3Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                DES(Out, In, &m_SubKey[bType ? 2 : 0], bType);
                DES(Out, Out, &m_SubKey[1], !bType);
                DES(Out, Out, &m_SubKey[bType ? 0 : 2], bType);
            }
        }
    }
    else  			//CBC模式
    {
        unsigned char cvec[8] = { 0 };	//扭转向量
        unsigned char cvin[8] = { 0 };	//中间变量
        memcpy(cvec, des_iv, 8);
        if (nKey == 1)  	//单Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                if (bType == XENCRYPT)
                {
                    for (int j = 0; j < 8; ++j)  	//将输入与扭转变量异或
                    {
                        cvin[j] = In[j] ^ cvec[j];
                    }
                }
                else
                {
                    memcpy(cvin, In, 8);
                }
                DES(Out, cvin, &m_SubKey[0], bType);
                if (bType == XENCRYPT)
                {
                    memcpy(cvec, Out, 8);			//将输出设定为扭转变量
                }
                else
                {
                    for (int j = 0; j < 8; ++j)  	//将输出与扭转变量异或
                    {
                        Out[j] = Out[j] ^ cvec[j];
                    }
                    memcpy(cvec, cvin, 8);			//将输入设定为扭转变量
                }
            }
        }
        else if (nKey == 2)  	//3DES CBC 2Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                if (bType == XENCRYPT)
                {
                    for (int j = 0; j < 8; ++j)  	//将输入与扭转变量异或
                    {
                        cvin[j] = In[j] ^ cvec[j];
                    }
                }
                else
                {
                    memcpy(cvin, In, 8);
                }
                DES(Out, cvin, &m_SubKey[0], bType);
                DES(Out, Out, &m_SubKey[1], !bType);
                DES(Out, Out, &m_SubKey[0], bType);
                if (bType == XENCRYPT)
                {
                    memcpy(cvec, Out, 8);			//将输出设定为扭转变量
                }
                else
                {
                    for (int j = 0; j < 8; ++j)  	//将输出与扭转变量异或
                    {
                        Out[j] = Out[j] ^ cvec[j];
                    }
                    memcpy(cvec, cvin, 8);			//将输入设定为扭转变量
                }
            }
        }
        else  		//3DES CBC 3Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                if (bType == XENCRYPT)
                {
                    for (int j = 0; j < 8; ++j)  	//将输入与扭转变量异或
                    {
                        cvin[j] = In[j] ^ cvec[j];
                    }
                }
                else
                {
                    memcpy(cvin, In, 8);
                }
                DES(Out, cvin, &m_SubKey[bType ? 2 : 0], bType);
                DES(Out, Out, &m_SubKey[1], !bType);
                DES(Out, Out, &m_SubKey[bType ? 0 : 2], bType);
                if (bType == XENCRYPT)
                {
                    memcpy(cvec, Out, 8);			//将输出设定为扭转变量
                }
                else
                {
                    for (int j = 0; j < 8; ++j)  	//将输出与扭转变量异或
                    {
                        Out[j] = Out[j] ^ cvec[j];
                    }
                    memcpy(cvec, cvin, 8);		//将输入设定为扭转变量
                }
            }
        }
    }
    return 1;
}

int DesDecrypt(char* sIn, char* sOut)
{
    char buffer[1024];
    unsigned inlen = string_to_value(sIn, buffer);
    unsigned char* In = (unsigned char*)buffer;
    unsigned char* Out = (unsigned char*)sOut;
    unsigned char* out_back = Out;
    int bType = XDECRYPT;
    unsigned char* Key = des_key;
    int keylen = 8;
    //判断输入合法性
    if (!(In && Out && Key && inlen && keylen >= 8))
    {
        return 0;
    }
    //只处理8的整数倍，不足长度自己填充
    if (inlen & 0x00000007)
    {
        return 0;
    }
    unsigned char m_SubKey[3][16][48];		//秘钥
    //构造并生成SubKeys
    unsigned char nKey = (keylen >> 3) > 3 ? 3 : (keylen >> 3);
    for (int i = 0; i < nKey; i++)
    {
        SetSubKey(&m_SubKey[i], &Key[i << 3]);
    }
    if (des_enmode == ECB)  	//ECB模式
    {
        if (nKey == 1)  	//单Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                DES(Out, In, &m_SubKey[0], bType);
            }
        }
        else if (nKey == 2)  	//3DES 2Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                DES(Out, In, &m_SubKey[0], bType);
                DES(Out, Out, &m_SubKey[1], !bType);
                DES(Out, Out, &m_SubKey[0], bType);
            }
        }
        else  		//3DES 3Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                DES(Out, In, &m_SubKey[bType ? 2 : 0], bType);
                DES(Out, Out, &m_SubKey[1], !bType);
                DES(Out, Out, &m_SubKey[bType ? 0 : 2], bType);
            }
        }
    }
    else  			//CBC模式
    {
        unsigned char cvec[8] = { 0 };	//扭转向量
        unsigned char cvin[8] = { 0 };	//中间变量
        memcpy(cvec, des_iv, 8);
        if (nKey == 1)  	//单Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                if (bType == XENCRYPT)
                {
                    for (int j = 0; j < 8; ++j)  	//将输入与扭转变量异或
                    {
                        cvin[j] = In[j] ^ cvec[j];
                    }
                }
                else
                {
                    memcpy(cvin, In, 8);
                }
                DES(Out, cvin, &m_SubKey[0], bType);
                if (bType == XENCRYPT)
                {
                    memcpy(cvec, Out, 8);			//将输出设定为扭转变量
                }
                else
                {
                    int j;
                    for (j = 0; j < 8; ++j)  	//将输出与扭转变量异或
                    {
                        Out[j] = Out[j] ^ cvec[j];
                    }
                    Out[j] = 0;
                    memcpy(cvec, cvin, 8);			//将输入设定为扭转变量
                }
            }
        }
        else if (nKey == 2)  	//3DES CBC 2Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                if (bType == XENCRYPT)
                {
                    for (int j = 0; j < 8; ++j)  	//将输入与扭转变量异或
                    {
                        cvin[j] = In[j] ^ cvec[j];
                    }
                }
                else
                {
                    memcpy(cvin, In, 8);
                }
                DES(Out, cvin, &m_SubKey[0], bType);
                DES(Out, Out, &m_SubKey[1], !bType);
                DES(Out, Out, &m_SubKey[0], bType);
                if (bType == XENCRYPT)
                {
                    memcpy(cvec, Out, 8);			//将输出设定为扭转变量
                }
                else
                {
                    for (int j = 0; j < 8; ++j)  	//将输出与扭转变量异或
                    {
                        Out[j] = Out[j] ^ cvec[j];
                    }
                    memcpy(cvec, cvin, 8);			//将输入设定为扭转变量
                }
            }
        }
        else  		//3DES CBC 3Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                if (bType == XENCRYPT)
                {
                    for (int j = 0; j < 8; ++j)  	//将输入与扭转变量异或
                    {
                        cvin[j] = In[j] ^ cvec[j];
                    }
                }
                else
                {
                    memcpy(cvin, In, 8);
                }
                DES(Out, cvin, &m_SubKey[bType ? 2 : 0], bType);
                DES(Out, Out, &m_SubKey[1], !bType);
                DES(Out, Out, &m_SubKey[bType ? 0 : 2], bType);
                if (bType == XENCRYPT)
                {
                    memcpy(cvec, Out, 8);			//将输出设定为扭转变量
                }
                else
                {
                    for (int j = 0; j < 8; ++j)  	//将输出与扭转变量异或
                    {
                        Out[j] = Out[j] ^ cvec[j];
                    }
                    memcpy(cvec, cvin, 8);		//将输入设定为扭转变量
                }
            }
        }
    }
    ClearPad(des_padmode, out_back, strlen((char*)out_back), out_back);
    return 1;
}


int DesDecrypt2(char* sIn, int inlen, char* sOut)
{
    unsigned char* In = (unsigned char*)sIn;
    unsigned char* Out = (unsigned char*)sOut;
    unsigned char* out_back = Out;
    int bType = XDECRYPT;
    unsigned char* Key = des_key;
    int keylen = 8;
    //判断输入合法性
    if (!(In && Out && Key && inlen && keylen >= 8))
    {
        return 0;
    }
    //只处理8的整数倍，不足长度自己填充
    if (inlen & 0x00000007)
    {
        return 0;
    }
    unsigned char m_SubKey[3][16][48];		//秘钥
    //构造并生成SubKeys
    unsigned char nKey = (keylen >> 3) > 3 ? 3 : (keylen >> 3);
    for (int i = 0; i < nKey; i++)
    {
        SetSubKey(&m_SubKey[i], &Key[i << 3]);
    }
    if (des_enmode == ECB)  	//ECB模式
    {
        if (nKey == 1)  	//单Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                DES(Out, In, &m_SubKey[0], bType);
            }
        }
        else if (nKey == 2)  	//3DES 2Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                DES(Out, In, &m_SubKey[0], bType);
                DES(Out, Out, &m_SubKey[1], !bType);
                DES(Out, Out, &m_SubKey[0], bType);
            }
        }
        else  		//3DES 3Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                DES(Out, In, &m_SubKey[bType ? 2 : 0], bType);
                DES(Out, Out, &m_SubKey[1], !bType);
                DES(Out, Out, &m_SubKey[bType ? 0 : 2], bType);
            }
        }
    }
    else  			//CBC模式
    {
        unsigned char cvec[8] = { 0 };	//扭转向量
        unsigned char cvin[8] = { 0 };	//中间变量
        memcpy(cvec, des_iv, 8);
        if (nKey == 1)  	//单Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                if (bType == XENCRYPT)
                {
                    for (int j = 0; j < 8; ++j)  	//将输入与扭转变量异或
                    {
                        cvin[j] = In[j] ^ cvec[j];
                    }
                }
                else
                {
                    memcpy(cvin, In, 8);
                }
                DES(Out, cvin, &m_SubKey[0], bType);
                if (bType == XENCRYPT)
                {
                    memcpy(cvec, Out, 8);			//将输出设定为扭转变量
                }
                else
                {
                    int j;
                    for (j = 0; j < 8; ++j)  	//将输出与扭转变量异或
                    {
                        Out[j] = Out[j] ^ cvec[j];
                    }
                    Out[j] = 0;
                    memcpy(cvec, cvin, 8);			//将输入设定为扭转变量
                }
            }
        }
        else if (nKey == 2)  	//3DES CBC 2Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                if (bType == XENCRYPT)
                {
                    for (int j = 0; j < 8; ++j)  	//将输入与扭转变量异或
                    {
                        cvin[j] = In[j] ^ cvec[j];
                    }
                }
                else
                {
                    memcpy(cvin, In, 8);
                }
                DES(Out, cvin, &m_SubKey[0], bType);
                DES(Out, Out, &m_SubKey[1], !bType);
                DES(Out, Out, &m_SubKey[0], bType);
                if (bType == XENCRYPT)
                {
                    memcpy(cvec, Out, 8);			//将输出设定为扭转变量
                }
                else
                {
                    for (int j = 0; j < 8; ++j)  	//将输出与扭转变量异或
                    {
                        Out[j] = Out[j] ^ cvec[j];
                    }
                    memcpy(cvec, cvin, 8);			//将输入设定为扭转变量
                }
            }
        }
        else  		//3DES CBC 3Key
        {
            for (int i = 0, j = inlen >> 3; i < j; ++i, Out += 8, In += 8)
            {
                if (bType == XENCRYPT)
                {
                    for (int j = 0; j < 8; ++j)  	//将输入与扭转变量异或
                    {
                        cvin[j] = In[j] ^ cvec[j];
                    }
                }
                else
                {
                    memcpy(cvin, In, 8);
                }
                DES(Out, cvin, &m_SubKey[bType ? 2 : 0], bType);
                DES(Out, Out, &m_SubKey[1], !bType);
                DES(Out, Out, &m_SubKey[bType ? 0 : 2], bType);
                if (bType == XENCRYPT)
                {
                    memcpy(cvec, Out, 8);			//将输出设定为扭转变量
                }
                else
                {
                    for (int j = 0; j < 8; ++j)  	//将输出与扭转变量异或
                    {
                        Out[j] = Out[j] ^ cvec[j];
                    }
                    memcpy(cvec, cvin, 8);		//将输入设定为扭转变量
                }
            }
        }
    }
    ClearPad(des_padmode, out_back, strlen((char*)out_back), out_back);
    return 1;
}

}

