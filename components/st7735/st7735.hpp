#ifndef __ST7735_H__
#define __ST7735_H__

#include <string.h>
#include <stdint.h>
#include <endian.h>
#include <initializer_list>
#include "spi.hpp"
#include "stdfonts.hpp"

class ST7735Display: public SpiMaster
{
public:
    struct PinCfg
    {
        SpiPinCfg spi;
        uint8_t dc; // data/command
        uint8_t rst;
    };
    enum { kMaxTransferLen = 64 };
    typedef int16_t coord_t;
    enum Orientation
    {
      kOrientNormal = 0,
      kOrientCW     = 1,
      kOrientCCW    = 2,
      kOrient180    = 3
    };
    enum DrawFlags
    {
        kFlagNoAutoNewline = 1,
        kFlagAllowPartial = 2
    };
protected:
    int16_t mWidth;
    int16_t mHeight;
    uint8_t mRstPin;
    uint8_t mDcPin;
    uint16_t mBgColor = 0x0000;
    uint16_t mFgColor = 0xffff;
    const Font* mFont = &Font_5x7;
    uint8_t mFontScale = 1;
    void setRstLevel(int level);
    void setDcPin(int level);
    inline void execTransaction();
    void displayReset();
public:
    int16_t cursorX = 0;
    int16_t cursorY = 0;
    int16_t width() const { return mWidth; }
    int16_t height() const { return mHeight; }
    const Font* font() const { return mFont; }
    static void usDelay(uint32_t us);
    static void msDelay(uint32_t ms);
    static uint16_t rgb(uint8_t R, uint8_t G, uint8_t B);
    ST7735Display(uint8_t spiHost);
    void setFgColor(uint16_t color) { mFgColor = htobe16(color); }
    void setFgColor(uint8_t r, uint8_t g, uint8_t b) { setFgColor(rgb(r, g, b)); }
    void setBgColor(uint16_t color) { mBgColor = htobe16(color); }
    void setBgColor(uint8_t r, uint8_t g, uint8_t b) { setBgColor(rgb(r, g, b)); }

    void gotoXY(int16_t x, int16_t y) { cursorX = x; cursorY = y; }
    void init(int16_t width, int16_t height, const PinCfg& pins);
    void sendCmd(uint8_t opcode);
    void sendCmd(uint8_t opcode, const std::initializer_list<uint8_t>& data);
    template <typename T>
    void sendCmd(uint8_t opcode, T data)
    {
        waitDone();
        sendCmd(opcode);
        sendData(data);
    }

    void sendData(const void* data, int size);
    void sendData(const std::initializer_list<uint8_t>& data) {
        sendData(data.begin(), (int)data.size());
    }
    template<typename T>
    void sendData(T data)
    {
        waitDone();
        setDcPin(1);
        spiSendVal(data);
    }
    void prepareSendPixels();
    void sendNextPixel(uint16_t pixel);
    void setOrientation(Orientation orientation);
    void setWriteWindow(uint16_t XS, uint16_t YS, uint16_t w, uint16_t h);
    void setWriteWindowCoords(uint16_t XS, uint16_t YS, uint16_t XE, uint16_t YE);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h) { fillRect(x, y, w, h, mFgColor); }
    void fillRect(int16_t x0, int16_t y0, int16_t w, int16_t h, uint16_t color);
    void clear() { fillRect(0, 0, mWidth, mHeight, mBgColor); }
    void clear(int16_t x, int16_t y, int16_t w, int16_t h) { fillRect(x, y, w, h, mBgColor); }
    void setPixel(uint16_t x, uint16_t y, uint16_t color);
    void hLine(uint16_t x1, uint16_t x2, uint16_t y);
    void vLine(uint16_t x, uint16_t y1, uint16_t y2);
    void rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
    void line(int16_t x1, int16_t y1, int16_t x2, int16_t y2);
    void blitMonoHscan(int16_t sx, int16_t sy, int16_t w, int16_t h, const uint8_t* binData, int8_t bgSpacing=0, int scale=1);
    void blitMonoVscan(int16_t sx, int16_t sy, int16_t w, int16_t h, const uint8_t* binData, int8_t bgSpacing=0, int scale=1);
    void setFont(const Font& font, int8_t scale=1) { mFont = &font; mFontScale = scale; }
    void setFontScale(int8_t scale) { mFontScale = scale; }
    int8_t charWidth() const { return (mFont->width + mFont->charSpacing) * mFontScale; }
    int8_t charHeight() const { return (mFont->height + mFont->lineSpacing) * mFontScale; }
    int8_t charsPerLine() const { return mWidth / charWidth(); }
    bool putc(uint8_t ch, uint8_t flags = 0, uint8_t startCol=0);
    void puts(const char* str, uint8_t flags = 0);
    void nputs(const char* str, int len, uint8_t flag=0);
    void gotoNextChar();
    void gotoNextLine();
};

// Some ready-made 16-bit ('565') color settings:
#define ST77XX_BLACK 0x0000
#define ST77XX_WHITE 0xFFFF
#define ST77XX_RED 0xF800
#define ST77XX_GREEN 0x07E0
#define ST77XX_BLUE 0x001F
#define ST77XX_CYAN 0x07FF
#define ST77XX_MAGENTA 0xF81F
#define ST77XX_YELLOW 0xFFE0
#define ST77XX_ORANGE 0xFC00

#endif
