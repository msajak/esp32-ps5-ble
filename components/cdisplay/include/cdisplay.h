#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

//********************************************
// This example is a basic sketch using the ESP32C3 micro board with a 0.42" OLED LCD
// To make it work, you need to have the espressif system installed and use the ESP32C3 Dev Module.
// Remember, for Serial to work, you need to enable it in arduino/tools/USB CDC on boot
// the screen is 72*40 pixels, 2 color
// Instanciate CDisplay and call begin to start it...
//
// CDisplay is framebuffer based.
// This means that you draw. And then you call the disp() function which will update the driver.
// Note that NONE of the function do ANY clipping! Do not overstep or you will crash! Ask me if you want a clipped version....
//
// This means that you can go modify the data there directly and then get them to display.
// At this point in time, the framebuffer still uses the original SSD1306 framebuffer structure which is weired and bad...
// The best way to understand it is probalby to try to understand the pixon function..
// Basically, it's set of 8 rows, with each byte being 8 pixel set in a vertical patern...
//
// you can then use the following functions:
//  void disp() -> send the whole framebuffer to the screen. Returns when completed. takes around 15ms which is 66 frames per second...
//  void screenOn() -> turn the screen on
//  void screenOff() -> What do you think?
//  void pixon(int8_t x, int8_t y) -> light a pixel
//  void pixoff(int8_t x, int8_t y) -> turns a pixel off
//  void hline(int8_t x, uint8_t w, int8_t y, bool on=true) -> draw an horizontal line. if on=false, erases it...
//  void vline(int8_t x, int8_t y, int8_t h, bool on=true) -> draws a vertical line. if on=false, erases it...
//  void rect(int8_t x, int8_t y, uint8_t w, int8_t h, bool on=true) -> draws a filled rectangle. if on=false, erases it...
//  uint8_t text(char const *s, int8_t x, int8_t y, uint8_t eraseLastCol= 1, int8_t nb=127) // display in 8*6 pixel font. if eraseLastCol=0 then the last character only displays 5 columns
//  uint8_t text2(char const *s, int8_t x, int8_t y, uint8_t eraseLastCol= 1, int8_t nb=127)  // display in 16*10 pixel font. if eraseLastCol=0 then the last character only displays 10 columns
//  void blit(uint8_t const *b, uint8_t x, uint8_t y) -> b points to a struct which has wifth, height and then a pixel map of what to display...

// pint 0-4 free to use (can be analog)
// static const int8_t LCDSDA    = 5;  // Data
// static const int8_t LCDSLC    = 6;  // clock
// pint 7 free to use
// static const int8_t LED    = 8;     // LED
// pint 9-10 free to use
// static const int8_t RX= 20;
// static const int8_t TX= 21;


class CDisplay {
public:
    static int const W= 72, H= 40;
    static int const ColumnsPlus1= 73;
    uint8_t *fb= FB+1;             // 360 bytes of RAM for the frame buffer. by packs of 8 lines wih 1 byte = 8 vertical pixels!!!! crapy formatting!!!!
    void clear(uint8_t v=0) { memset(fb, v, sizeof(FB) - 1); }

    void begin()
    {
        i2c_config_t conf = {};
        conf.mode = I2C_MODE_MASTER;
        conf.sda_io_num = I2C_SDA;
        conf.scl_io_num = I2C_SCL;
        conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
        conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
        conf.master.clk_speed = I2C_FREQ;
        ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &conf));
        ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

        static uint8_t const initSeq[]= // 1 byte length of data, followed by data to be sent???
        { 2, 0x00, 0xAE,
          3, 0x00, 0xD5, 0x80,
          3, 0x00, 0xA8, 0x3F,
          3, 0x00, 0xD3, 0x00,
          2, 0x00, 0x40,
          3, 0x00, 0x8D, 0x14,
          3, 0x00, 0x20, 0x00,
          2, 0x00, 0xA1,
          2, 0x00, 0xC8,
          3, 0x00, 0xDA, 0x12,
          3, 0x00, 0x81, 0xCF, // contrast. Is crap on this display anyway :-(
          3, 0x00, 0xD9, 0xF1,
          3, 0x00, 0xDB, 0x40,
          2, 0x00, 0x2E,
          2, 0x00, 0xA4,
          2, 0x00, 0xA6,
          2, 0x00, 0x7f-11, // 0x40+12, // first line is 12! Command is 0x40 but shifted by -12 so 7f-11... Read the doc... and guess!
          2, 0x00, 0xAF, // screen on
          0};
        uint8_t const *t= initSeq;
        while (*t!=0) { int l= *t++; send(t, l); t+= l; } // send initialisation sequence...
    }

    void disp() // send the whole screen to the driver and wait until sent to continue...
    {
        uint8_t c[4]= { 0x00, 0x11, 0x0e, 0xB0}; // start writing at column 30. row 0 (but we have a WH scroll of 12)
        for (int i=0; i<5; i++)
        {
          c[3]= 0xB0+i; send(c, 4);
          FB[i*ColumnsPlus1]= 0x40; send(FB+i*ColumnsPlus1, 72+1);
        }
    }
    void screenOn() { static uint8_t const cmd[2]= { 0, 0xAF }; send(cmd, 2); }
    void screenOff() { static uint8_t const cmd[2]= { 0, 0xAE }; send(cmd, 2); }
    // Framebuffer manipulations
    // The organization in the framebuffer is non-trivial and non-intuitive it's by set of 8 rows, but with 1 byte = 1 colum of 8 pixels...
    // Note that none of these function do any cliping!
    bool pixel(int8_t x, int8_t y) { return (fb[x+(y>>3)*ColumnsPlus1] & (1<<(y&7)))!=0; }
    void pixon(int8_t x, int8_t y) { fb[x+(y>>3)*ColumnsPlus1]|= 1<<(y&7); }
    void pixoff(int8_t x, int8_t y) { fb[x+(y>>3)*ColumnsPlus1]&= ~(1<<(y&7)); }
    void hline(int8_t x, uint8_t w, int8_t y, bool on=true) // Horizontal line
    {
        uint8_t m= 1<<(y&7);
        uint8_t *d= fb+x+(y>>3)*ColumnsPlus1;
        if (on) while (w!=0) { w--; *d++|= m; } else { m= ~m; while (w!=0) { w--; *d++&=m; }  }
    }
    void rect(int8_t x, int8_t y, uint8_t w, int8_t h, bool on=true) { while (--h>=0) hline(x, w, y++, on); }
    void vline(int8_t x, int8_t y, int8_t h, bool on=true) // Vertical lune // could be speed on...
    { if (on) while (h!=0) { h--; pixon(x, y++); } else while (h!=0) { h--; pixoff(x, y++); } }

    uint8_t text(char const *s, int8_t x, int8_t y, uint8_t eraseLastCol= 1, int8_t nb=127) // display in 8*6 pixel font. if eraseLastCol=0 then the last character only displays 5 columns
    {
        while (*s!=0 && --nb>=0)
        {
            char c= *s++;
            if (eraseLastCol==0 && *s==0) eraseLastCol= 3;
            x= dspChar(c, x, y, eraseLastCol);
        }
        return x;
    }
    uint8_t text2(char const *s, int8_t x, int8_t y, uint8_t eraseLastCol= 1, int8_t nb=127)  // display in 16*10 pixel font. if eraseLastCol=0 then the last character only displays 10 columns
    {
        while (*s!=0 && --nb>=0)
        {
            char c= *s++;
            if (eraseLastCol==0 && (nb==0 || *s==0)) eraseLastCol= 3;
            x= dspChar2(c, x, y, eraseLastCol);
        }
        return x;
    }
    // b is in PROGMEM and is width, height and then the bitmap bits, vertically, with the next column starting as soon as the previous is done (no empty space)
    void blit(uint8_t const *b, uint8_t x, uint8_t y)
    {
        int8_t w= *(b++);
        int8_t h= *(b++); uint8_t B; int8_t bs= 1;
        uint8_t *P= fb+x+(y>>3)*ColumnsPlus1; y= 1<<(y&7);
        while (--w>=0)
        {
            uint8_t *p= P; uint8_t v= *p; uint8_t m= y; int8_t r= h;
            while (true)
            {
                if (--bs==0) { bs= 8; B= *(b++); }
                if ((B&1)==0) v&= ~m; else v|= m;
                B>>=1;
                if (--r==0) break;
                m<<=1; if (m==0) { *p= v; p+=ColumnsPlus1; m= 1; v= *p; }
            }
            *p= v;
            P++;
        }

    }
  private:
    static constexpr i2c_port_t I2C_PORT = I2C_NUM_0;
    static constexpr gpio_num_t I2C_SDA = GPIO_NUM_5;
    static constexpr gpio_num_t I2C_SCL = GPIO_NUM_6;
    static constexpr uint32_t I2C_FREQ = 200000;
    static constexpr uint8_t I2C_ADDR = 0x3C;
    static constexpr uint8_t const font8[] = { // 6*8 pixel font with only chr 32 to 127
        /* */ 0,0,0,0,0,0,0,0, /*!*/ 4,4,4,4,4,0,4,0, /*"*/ 10,10,10,0,0,0,0,0, /*#*/ 10,10,31,10,31,10,10,0, /*$*/ 4,30,5,14,20,15,4,0, /*%*/ 3,19,8,4,2,25,24,0, /*&*/ 2,5,5,2,21,9,22,0, /*'*/ 4,4,4,0,0,0,0,0, /*(*/ 8,4,2,2,2,4,8,0, /*)*/ 2,4,8,8,8,4,2,0, /***/ 0,10,4,31,4,10,0,0, /*+*/ 0,4,4,31,4,4,0,0, /*,*/ 0,0,0,0,6,6,4,2, /*-*/ 0,0,0,31,0,0,0,0, /*.*/ 0,0,0,0,0,6,6,0, /*/*/ 0,16,8,4,2,1,0,0,
        /*0*/ 14,17,25,21,19,17,14,0, /*1*/ 4,6,4,4,4,4,14,0, /*2*/ 14,17,16,12,2,1,31,0, /*3*/ 14,17,16,14,16,17,14,0, /*4*/ 8,12,10,9,31,8,8,0, /*5*/ 31,1,15,16,16,17,14,0, /*6*/ 12,2,1,15,17,17,14,0, /*7*/ 31,16,8,4,2,2,2,0, /*8*/ 14,17,17,14,17,17,14,0, /*9*/ 14,17,17,30,16,8,6,0, /*:*/ 0,6,6,0,6,6,0,0, /*;*/ 0,6,6,0,6,6,4,2, /*<*/ 8,4,2,1,2,4,8,0, /*=*/ 0,0,31,0,31,0,0,0, /*>*/ 1,2,4,8,4,2,1,0, /*?*/ 14,17,16,8,4,0,4,0,
        /*@*/ 14,17,21,29,5,1,30,0, /*A*/ 14,17,17,31,17,17,17,0, /*B*/ 15,17,17,15,17,17,15,0, /*C*/ 14,17,1,1,1,17,14,0, /*D*/ 7,9,17,17,17,9,7,0, /*E*/ 31,1,1,15,1,1,31,0, /*F*/ 31,1,1,15,1,1,1,0, /*G*/ 14,17,1,1,25,17,30,0, /*H*/ 17,17,17,31,17,17,17,0, /*I*/ 14,4,4,4,4,4,14,0, /*J*/ 16,16,16,16,17,17,14,0, /*K*/ 17,9,5,3,5,9,17,0, /*L*/ 1,1,1,1,1,1,31,0, /*M*/ 17,27,21,21,17,17,17,0, /*N*/ 17,17,19,21,25,17,17,0, /*O*/ 14,17,17,17,17,17,14,0,
        /*P*/ 15,17,17,15,1,1,1,0, /*Q*/ 14,17,17,17,21,9,22,0, /*R*/ 15,17,17,15,5,9,17,0, /*S*/ 14,17,1,14,16,17,14,0, /*T*/ 31,4,4,4,4,4,4,0, /*U*/ 17,17,17,17,17,17,14,0, /*V*/ 17,17,17,10,10,4,4,0, /*W*/ 17,17,17,21,21,27,17,0, /*X*/ 17,17,10,4,10,17,17,0, /*Y*/ 17,17,10,4,4,4,4,0, /*Z*/ 31,16,8,4,2,1,31,0, /*[*/ 14,2,2,2,2,2,14,0, /*\*/ 0,1,2,4,8,16,0,0, /*]*/ 14,8,8,8,8,8,14,0, /*^*/ 4,10,17,0,0,0,0,0, /*_*/ 0,0,0,0,0,0,31,0,
        /*`*/ 2,2,4,0,0,0,0,0, /*a*/ 0,0,14,16,30,17,30,0, /*b*/ 1,1,15,17,17,17,15,0, /*c*/ 0,0,30,1,1,1,30,0, /*d*/ 16,16,30,17,17,17,30,0, /*e*/ 0,0,14,17,31,1,14,0, /*f*/ 4,10,2,7,2,2,2,0, /*g*/ 0,0,14,17,17,30,16,14, /*h*/ 1,1,15,17,17,17,17,0, /*i*/ 4,0,6,4,4,4,14,0, /*j*/ 8,0,12,8,8,8,9,6, /*k*/ 1,1,9,5,3,5,9,0, /*l*/ 6,4,4,4,4,4,14,0, /*m*/ 0,0,11,21,21,21,17,0, /*n*/ 0,0,15,17,17,17,17,0, /*o*/ 0,0,14,17,17,17,14,0,
        /*p*/ 0,0,15,17,17,15,1,1, /*q*/ 0,0,30,17,17,30,16,16, /*r*/ 0,0,29,3,1,1,1,0, /*s*/ 0,0,30,1,14,16,15,0, /*t*/ 2,2,7,2,2,10,4,0, /*u*/ 0,0,17,17,17,17,30,0, /*v*/ 0,0,17,17,17,10,4,0, /*w*/ 0,0,17,17,21,21,10,0, /*x*/ 0,0,17,10,4,10,17,0, /*y*/ 0,0,17,17,17,30,16,14, /*z*/ 0,0,31,8,4,2,31,0, /*{*/ 12,2,2,1,2,2,12,0, /*|*/ 4,4,4,4,4,4,4,0, /*}*/ 6,8,8,16,8,8,6,0, /*~*/ 0,0,2,21,8,0,0,0, /*âŒ‚*/ 7,5,7,0,0,0,0,0,
    };

    uint8_t dspChar(char s, uint8_t x, uint8_t y, uint8_t eraseLastCol= 1) // disp 1 8*6 pixel character
    {
        uint8_t const *f= font8+(s-32)*8;
        uint8_t h= 8; if (y+8>=40) h= 40-y;
        for (int i=0; i<h; i++)
        {
            uint8_t m= 1<<((y+i)&7);
            uint8_t *p= fb+x+((y+i)>>3)*ColumnsPlus1;
            uint8_t v= *(f+i);
            if ((v&1)!=0)  *p++|=m; else *p++&=~m;
            if ((v&2)!=0)  *p++|=m; else *p++&=~m;
            if ((v&4)!=0)  *p++|=m; else *p++&=~m;
            if ((v&8)!=0)  *p++|=m; else *p++&=~m;
            if ((v&16)!=0) *p++|=m; else *p++&=~m;
            if (eraseLastCol!=3) {
                if ((v&32)!=0) { *p++|=m; }
                else { *p++&=~m; }
            }
        }
        return x+6;
    }
    uint8_t dspChar2(char s, uint8_t x, uint8_t y, uint8_t eraseLastCol= 1) // displ 1 16*12 pixels character
    {
        uint8_t const *f= font8+(s-32)*8;
        uint8_t h= 16; if (y+16>=40) h= 40-y;
        for (int8_t i=0; i<h; i++)
        {
            uint8_t m= 1<<((y+i)&7);
            uint8_t *p= fb+x+((y+i)>>3)*ColumnsPlus1;
            uint8_t v= *(f+(i>>1));
            if ((v&1)!=0) { *p++|=m; *p++|=m; } else { *p++&=~m; *p++&=~m; }
            if ((v&2)!=0) { *p++|=m; *p++|=m; } else { *p++&=~m; *p++&=~m; }
            if ((v&4)!=0) { *p++|=m; *p++|=m; } else { *p++&=~m; *p++&=~m; }
            if ((v&8)!=0) { *p++|=m; *p++|=m; } else { *p++&=~m; *p++&=~m; }
            if ((v&16)!=0) { *p++|=m; *p++|=m; } else { *p++&=~m; *p++&=~m; }
            if (eraseLastCol!=3)  {
                if ((v&32)!=0) { *p++|=m; *p++|=m; }
                else { *p++&=~m; *p++&=~m; }
            }
        }
        return x+12;
    }
    uint8_t FB[ColumnsPlus1*40/8]; // ok, so this is here, and fb is actually 1 byte after to allow for I2C data to be placed JUST before the framebuffer!
                                   // When sending I2C data, the system will use the byte just before the start of each pack of 8 rows to add some data that needs to go to the LCD driver...

    static void send(uint8_t const *i, int16_t nb) // Send an I2C packet...
    {
      esp_err_t err = i2c_master_write_to_device(I2C_PORT, I2C_ADDR, i, nb, pdMS_TO_TICKS(1000));
      if (err != ESP_OK) {
          ESP_LOGE("CDisplay", "I2C write failed: %s", esp_err_to_name(err));
      }
    }
};
