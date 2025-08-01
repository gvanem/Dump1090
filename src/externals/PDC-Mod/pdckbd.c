#include <curspriv.h>

/* These variables are used to store information about the next
   Input Event.
 */

static INPUT_RECORD save_ip;
static DWORD event_count = 0;
static SHORT left_key;
static int key_count = 0;
static int save_press = 0;

#define KEV save_ip.Event.KeyEvent
#define MEV save_ip.Event.MouseEvent
#define REV save_ip.Event.WindowBufferSizeEvent

/*
 * Table for key code translation of function keys in keypad mode
 * These values are for strict IBM keyboard compatibles only
 */
typedef struct {
        unsigned short normal;
        unsigned short shift;
        unsigned short control;
        unsigned short alt;
        unsigned short extended;
      } KPTAB;

static KPTAB kptab[] = {
   {0,          0,         0,           0,          0   }, /* 0  */
   {0,          0,         0,           0,          0   }, /* 1   VK_LBUTTON */
   {0,          0,         0,           0,          0   }, /* 2   VK_RBUTTON */
   {0,          0,         0,           0,          0   }, /* 3   VK_CANCEL  */
   {0,          0,         0,           0,          0   }, /* 4   VK_MBUTTON */
   {0,          0,         0,           0,          0   }, /* 5   */
   {0,          0,         0,           0,          0   }, /* 6   */
   {0,          0,         0,           0,          0   }, /* 7   */
   {0x08,       0x08,      0x7F,        ALT_BKSP,   0   }, /* 8   VK_BACK    */
   {0x09,       KEY_BTAB,  CTL_TAB,     ALT_TAB,    999 }, /* 9   VK_TAB     */
   {0,          0,         0,           0,          0   }, /* 10  */
   {0,          0,         0,           0,          0   }, /* 11  */
   {KEY_B2,     0x35,      CTL_PAD5,    ALT_PAD5,   0   }, /* 12  VK_CLEAR   */
   {0x0D,       0x0D,      CTL_ENTER,   ALT_ENTER,  1   }, /* 13  VK_RETURN  */
   {0,          0,         0,           0,          0   }, /* 14  */
   {0,          0,         0,           0,          0   }, /* 15  */
   {0,          0,         0,           0,          0   }, /* 16  VK_SHIFT   HANDLED SEPARATELY */
   {0,          0,         0,           0,          0   }, /* 17  VK_CONTROL HANDLED SEPARATELY */
   {0,          0,         0,           0,          0   }, /* 18  VK_MENU    HANDLED SEPARATELY */
   {0,          0,         0,           0,          0   }, /* 19  VK_PAUSE   */
   {0,          0,         0,           0,          0   }, /* 20  VK_CAPITAL HANDLED SEPARATELY */
   {0,          0,         0,           0,          0   }, /* 21  VK_HANGUL  */
   {0,          0,         0,           0,          0   }, /* 22  */
   {0,          0,         0,           0,          0   }, /* 23  VK_JUNJA   */
   {0,          0,         0,           0,          0   }, /* 24  VK_FINAL   */
   {0,          0,         0,           0,          0   }, /* 25  VK_HANJA   */
   {0,          0,         0,           0,          0   }, /* 26  */
   {0x1B,       0x1B,      0x1B,        ALT_ESC,    0   }, /* 27  VK_ESCAPE  */
   {0,          0,         0,           0,          0   }, /* 28  VK_CONVERT */
   {0,          0,         0,           0,          0   }, /* 29  VK_NONCONVERT */
   {0,          0,         0,           0,          0   }, /* 30  VK_ACCEPT  */
   {0,          0,         0,           0,          0   }, /* 31  VK_MODECHANGE */
   {0x20,       0x20,      0x20,        0x20,       0   }, /* 32  VK_SPACE   */
   {KEY_A3,     0x39,      CTL_PAD9,    ALT_PAD9,   3   }, /* 33  VK_PRIOR   */
   {KEY_C3,     0x33,      CTL_PAD3,    ALT_PAD3,   4   }, /* 34  VK_NEXT    */
   {KEY_C1,     0x31,      CTL_PAD1,    ALT_PAD1,   5   }, /* 35  VK_END     */
   {KEY_A1,     0x37,      CTL_PAD7,    ALT_PAD7,   6   }, /* 36  VK_HOME    */
   {KEY_B1,     0x34,      CTL_PAD4,    ALT_PAD4,   7   }, /* 37  VK_LEFT    */
   {KEY_A2,     0x38,      CTL_PAD8,    ALT_PAD8,   8   }, /* 38  VK_UP      */
   {KEY_B3,     0x36,      CTL_PAD6,    ALT_PAD6,   9   }, /* 39  VK_RIGHT   */
   {KEY_C2,     0x32,      CTL_PAD2,    ALT_PAD2,   10  }, /* 40  VK_DOWN    */
   {0,          0,         0,           0,          0   }, /* 41  VK_SELECT  */
   {0,          0,         0,           0,          0   }, /* 42  VK_PRINT   */
   {0,          0,         0,           0,          0   }, /* 43  VK_EXECUTE */
   {0,          0,         0,           0,          0   }, /* 44  VK_SNAPSHOT*/
   {PAD0,       0x30,      CTL_PAD0,    ALT_PAD0,   11  }, /* 45  VK_INSERT  */
   {PADSTOP,    0x2E,      CTL_PADSTOP, ALT_PADSTOP,12  }, /* 46  VK_DELETE  */
   {0,          0,         0,           0,          0   }, /* 47  VK_HELP    */
   {0x30,       0x29,      '0',         ALT_0,      0   }, /* 48  */
   {0x31,       0x21,      '1',         ALT_1,      0   }, /* 49  */
   {0x32,       0x40,      '2',         ALT_2,      0   }, /* 50  */
   {0x33,       0x23,      '3',         ALT_3,      0   }, /* 51  */
   {0x34,       0x24,      '4',         ALT_4,      0   }, /* 52  */
   {0x35,       0x25,      '5',         ALT_5,      0   }, /* 53  */
   {0x36,       0x5E,      '6',         ALT_6,      0   }, /* 54  */
   {0x37,       0x26,      '7',         ALT_7,      0   }, /* 55  */
   {0x38,       0x2A,      '8',         ALT_8,      0   }, /* 56  */
   {0x39,       0x28,      '9',         ALT_9,      0   }, /* 57  */
   {0,          0,         0,           0,          0   }, /* 58  */
   {0,          0,         0,           0,          0   }, /* 59  */
   {0,          0,         0,           0,          0   }, /* 60  */
   {0,          0,         0,           0,          0   }, /* 61  */
   {0,          0,         0,           0,          0   }, /* 62  */
   {0,          0,         0,           0,          0   }, /* 63  */
   {0,          0,         0,           0,          0   }, /* 64  */
   {0x61,       0x41,      0x01,        ALT_A,      0   }, /* 65  */
   {0x62,       0x42,      0x02,        ALT_B,      0   }, /* 66  */
   {0x63,       0x43,      0x03,        ALT_C,      0   }, /* 67  */
   {0x64,       0x44,      0x04,        ALT_D,      0   }, /* 68  */
   {0x65,       0x45,      0x05,        ALT_E,      0   }, /* 69  */
   {0x66,       0x46,      0x06,        ALT_F,      0   }, /* 70  */
   {0x67,       0x47,      0x07,        ALT_G,      0   }, /* 71  */
   {0x68,       0x48,      0x08,        ALT_H,      0   }, /* 72  */
   {0x69,       0x49,      0x09,        ALT_I,      0   }, /* 73  */
   {0x6A,       0x4A,      0x0A,        ALT_J,      0   }, /* 74  */
   {0x6B,       0x4B,      0x0B,        ALT_K,      0   }, /* 75  */
   {0x6C,       0x4C,      0x0C,        ALT_L,      0   }, /* 76  */
   {0x6D,       0x4D,      0x0D,        ALT_M,      0   }, /* 77  */
   {0x6E,       0x4E,      0x0E,        ALT_N,      0   }, /* 78  */
   {0x6F,       0x4F,      0x0F,        ALT_O,      0   }, /* 79  */
   {0x70,       0x50,      0x10,        ALT_P,      0   }, /* 80  */
   {0x71,       0x51,      0x11,        ALT_Q,      0   }, /* 81  */
   {0x72,       0x52,      0x12,        ALT_R,      0   }, /* 82  */
   {0x73,       0x53,      0x13,        ALT_S,      0   }, /* 83  */
   {0x74,       0x54,      0x14,        ALT_T,      0   }, /* 84  */
   {0x75,       0x55,      0x15,        ALT_U,      0   }, /* 85  */
   {0x76,       0x56,      0x16,        ALT_V,      0   }, /* 86  */
   {0x77,       0x57,      0x17,        ALT_W,      0   }, /* 87  */
   {0x78,       0x58,      0x18,        ALT_X,      0   }, /* 88  */
   {0x79,       0x59,      0x19,        ALT_Y,      0   }, /* 89  */
   {0x7A,       0x5A,      0x1A,        ALT_Z,      0   }, /* 90  */
   {0,          0,         0,           0,          0   }, /* 91  VK_LWIN    */
   {0,          0,         0,           0,          0   }, /* 92  VK_RWIN    */
   {0,          0,         0,           0,          13  }, /* 93  VK_APPS    */
   {0,          0,         0,           0,          0   }, /* 94  */
   {0,          0,         0,           0,          0   }, /* 95  */
   {0x30,       0,         CTL_PAD0,    ALT_PAD0,   0   }, /* 96  VK_NUMPAD0 */
   {0x31,       0,         CTL_PAD1,    ALT_PAD1,   0   }, /* 97  VK_NUMPAD1 */
   {0x32,       0,         CTL_PAD2,    ALT_PAD2,   0   }, /* 98  VK_NUMPAD2 */
   {0x33,       0,         CTL_PAD3,    ALT_PAD3,   0   }, /* 99  VK_NUMPAD3 */
   {0x34,       0,         CTL_PAD4,    ALT_PAD4,   0   }, /* 100 VK_NUMPAD4 */
   {0x35,       0,         CTL_PAD5,    ALT_PAD5,   0   }, /* 101 VK_NUMPAD5 */
   {0x36,       0,         CTL_PAD6,    ALT_PAD6,   0   }, /* 102 VK_NUMPAD6 */
   {0x37,       0,         CTL_PAD7,    ALT_PAD7,   0   }, /* 103 VK_NUMPAD7 */
   {0x38,       0,         CTL_PAD8,    ALT_PAD8,   0   }, /* 104 VK_NUMPAD8 */
   {0x39,       0,         CTL_PAD9,    ALT_PAD9,   0   }, /* 105 VK_NUMPAD9 */
   {PADSTAR,   SHF_PADSTAR,CTL_PADSTAR, ALT_PADSTAR,999 }, /* 106 VK_MULTIPLY*/
   {PADPLUS,   SHF_PADPLUS,CTL_PADPLUS, ALT_PADPLUS,999 }, /* 107 VK_ADD     */
   {0,          0,         0,           0,          0   }, /* 108 VK_SEPARATOR     */
   {PADMINUS, SHF_PADMINUS,CTL_PADMINUS,ALT_PADMINUS,999}, /* 109 VK_SUBTRACT*/
   {0x2E,       0,         CTL_PADSTOP, ALT_PADSTOP,0   }, /* 110 VK_DECIMAL */
   {PADSLASH,  SHF_PADSLASH,CTL_PADSLASH,ALT_PADSLASH,2 }, /* 111 VK_DIVIDE  */
   {KEY_F(1),   KEY_F(13), KEY_F(25),   KEY_F(37),  0   }, /* 112 VK_F1      */
   {KEY_F(2),   KEY_F(14), KEY_F(26),   KEY_F(38),  0   }, /* 113 VK_F2      */
   {KEY_F(3),   KEY_F(15), KEY_F(27),   KEY_F(39),  0   }, /* 114 VK_F3      */
   {KEY_F(4),   KEY_F(16), KEY_F(28),   KEY_F(40),  0   }, /* 115 VK_F4      */
   {KEY_F(5),   KEY_F(17), KEY_F(29),   KEY_F(41),  0   }, /* 116 VK_F5      */
   {KEY_F(6),   KEY_F(18), KEY_F(30),   KEY_F(42),  0   }, /* 117 VK_F6      */
   {KEY_F(7),   KEY_F(19), KEY_F(31),   KEY_F(43),  0   }, /* 118 VK_F7      */
   {KEY_F(8),   KEY_F(20), KEY_F(32),   KEY_F(44),  0   }, /* 119 VK_F8      */
   {KEY_F(9),   KEY_F(21), KEY_F(33),   KEY_F(45),  0   }, /* 120 VK_F9      */
   {KEY_F(10),  KEY_F(22), KEY_F(34),   KEY_F(46),  0   }, /* 121 VK_F10     */
   {KEY_F(11),  KEY_F(23), KEY_F(35),   KEY_F(47),  0   }, /* 122 VK_F11     */
   {KEY_F(12),  KEY_F(24), KEY_F(36),   KEY_F(48),  0   }, /* 123 VK_F12     */

   /* 124 through 218 */

    {0,       0,      0,        0,       0   },  /* 7c 124 VK_F13 */
    {0,       0,      0,        0,       0   },  /* 7d 125 VK_F14 */
    {0,       0,      0,        0,       0   },  /* 7e 126 VK_F15 */
    {0,       0,      0,        0,       0   },  /* 7f 127 VK_F16 */
    {0,       0,      0,        0,       0   },  /* 80 128 VK_F17 */
    {0,       0,      0,        0,       0   },  /* 81 129 VK_F18 */
    {0,       0,      0,        0,       0   },  /* 82 130 VK_F19 */
    {0,       0,      0,        0,       0   },  /* 83 131 VK_F20 */
    {0,       0,      0,        0,       0   },  /* 84 132 VK_F21 */
    {0,       0,      0,        0,       0   },  /* 85 133 VK_F22 */
    {0,       0,      0,        0,       0   },  /* 86 134 VK_F23 */
    {0,       0,      0,        0,       0   },  /* 87 135 VK_F24 */

    {0, 0, 0, 0, 0},  /* 136 unassigned */
    {0, 0, 0, 0, 0},  /* 137 unassigned */
    {0, 0, 0, 0, 0},  /* 138 unassigned */
    {0, 0, 0, 0, 0},  /* 139 unassigned */
    {0, 0, 0, 0, 0},  /* 140 unassigned */
    {0, 0, 0, 0, 0},  /* 141 unassigned */
    {0, 0, 0, 0, 0},  /* 142 unassigned */
    {0, 0, 0, 0, 0},  /* 143 unassigned */
    {0, 0, 0, 0, 0},  /* 144 VK_NUMLOCK */
    {KEY_SCROLLLOCK, 0, 0, KEY_SCROLLLOCK, 0},    /* 145 VKSCROLL */
    {0, 0, 0, 0, 0},  /* 146 OEM specific */
    {0, 0, 0, 0, 0},  /* 147 OEM specific */
    {0, 0, 0, 0, 0},  /* 148 OEM specific */
    {0, 0, 0, 0, 0},  /* 149 OEM specific */
    {0, 0, 0, 0, 0},  /* 150 OEM specific */
    {0, 0, 0, 0, 0},  /* 151 Unassigned */
    {0, 0, 0, 0, 0},  /* 152 Unassigned */
    {0, 0, 0, 0, 0},  /* 153 Unassigned */
    {0, 0, 0, 0, 0},  /* 154 Unassigned */
    {0, 0, 0, 0, 0},  /* 155 Unassigned */
    {0, 0, 0, 0, 0},  /* 156 Unassigned */
    {0, 0, 0, 0, 0},  /* 157 Unassigned */
    {0, 0, 0, 0, 0},  /* 158 Unassigned */
    {0, 0, 0, 0, 0},  /* 159 Unassigned */
    {0, 0, 0, 0, 0},  /* 160 VK_LSHIFT */
    {0, 0, 0, 0, 0},  /* 161 VK_RSHIFT */
    {0, 0, 0, 0, 0},  /* 162 VK_LCONTROL */
    {0, 0, 0, 0, 0},  /* 163 VK_RCONTROL */
    {0, 0, 0, 0, 0},  /* 164 VK_LMENU */
    {0, 0, 0, 0, 0},  /* 165 VK_RMENU */
    {0, 0, 0, 0, 14},  /* 166 VK_BROWSER_BACK        */
    {0, 0, 0, 0, 15},  /* 167 VK_BROWSER_FORWARD     */
    {0, 0, 0, 0, 16},  /* 168 VK_BROWSER_REFRESH     */
    {0, 0, 0, 0, 17},  /* 169 VK_BROWSER_STOP        */
    {0, 0, 0, 0, 18},  /* 170 VK_BROWSER_SEARCH      */
    {0, 0, 0, 0, 19},  /* 171 VK_BROWSER_FAVORITES   */
    {0, 0, 0, 0, 20},  /* 172 VK_BROWSER_HOME        */
    {0, 0, 0, 0, 21},  /* 173 VK_VOLUME_MUTE         */
    {0, 0, 0, 0, 22},  /* 174 VK_VOLUME_DOWN         */
    {0, 0, 0, 0, 23},  /* 175 VK_VOLUME_UP           */
    {0, 0, 0, 0, 24},  /* 176 VK_MEDIA_NEXT_TRACK    */
    {0, 0, 0, 0, 25},  /* 177 VK_MEDIA_PREV_TRACK    */
    {0, 0, 0, 0, 26},  /* 178 VK_MEDIA_STOP          */
    {0, 0, 0, 0, 27},  /* 179 VK_MEDIA_PLAY_PAUSE    */
    {0, 0, 0, 0, 28},  /* 180 VK_LAUNCH_MAIL         */
    {0, 0, 0, 0, 29},  /* 181 VK_LAUNCH_MEDIA_SELECT */
    {0, 0, 0, 0, 30},  /* 182 VK_LAUNCH_APP1         */
    {0, 0, 0, 0, 31},  /* 183 VK_LAUNCH_APP2         */
    {0, 0, 0, 0, 0},  /* 184 Reserved */
    {0, 0, 0, 0, 0},  /* 185 Reserved */
    {';', ':', ';',           ALT_SEMICOLON, 0},  /* 186 VK_OEM_1      */
    {'=', '+', '=',           ALT_EQUAL,     0},  /* 187 VK_OEM_PLUS   */
    {',', '<', ',',           ALT_COMMA,     0},  /* 188 VK_OEM_COMMA  */
    {'-', '_', '-',           ALT_MINUS,     0},  /* 189 VK_OEM_MINUS  */
    {'.', '>', '.',           ALT_STOP,      0},  /* 190 VK_OEM_PERIOD */
    {'/', '?', '/',           ALT_FSLASH,    0},  /* 191 VK_OEM_2      */
    {'`', '~', '`',           ALT_BQUOTE,    0},  /* 192 VK_OEM_3      */
    {0, 0, 0, 0, 0},  /* 193 */
    {0, 0, 0, 0, 0},  /* 194 */
    {0, 0, 0, 0, 0},  /* 195 */
    {0, 0, 0, 0, 0},  /* 196 */
    {0, 0, 0, 0, 0},  /* 197 */
    {0, 0, 0, 0, 0},  /* 198 */
    {0, 0, 0, 0, 0},  /* 199 */
    {0, 0, 0, 0, 0},  /* 200 */
    {0, 0, 0, 0, 0},  /* 201 */
    {0, 0, 0, 0, 0},  /* 202 */
    {0, 0, 0, 0, 0},  /* 203 */
    {0, 0, 0, 0, 0},  /* 204 */
    {0, 0, 0, 0, 0},  /* 205 */
    {0, 0, 0, 0, 0},  /* 206 */
    {0, 0, 0, 0, 0},  /* 207 */
    {0, 0, 0, 0, 0},  /* 208 */
    {0, 0, 0, 0, 0},  /* 209 */
    {0, 0, 0, 0, 0},  /* 210 */
    {0, 0, 0, 0, 0},  /* 211 */
    {0, 0, 0, 0, 0},  /* 212 */
    {0, 0, 0, 0, 0},  /* 213 */
    {0, 0, 0, 0, 0},  /* 214 */
    {0, 0, 0, 0, 0},  /* 215 */
    {0, 0, 0, 0, 0},  /* 216 */
    {0, 0, 0, 0, 0},  /* 217 */
    {0, 0, 0, 0, 0},  /* 218 */
   {0x5B,       0x7B,      0x1B,        ALT_LBRACKET,0  }, /* 219 DB */
   {0x5C,       0x7C,      0x1C,        ALT_BSLASH, 0   }, /* 220 DC */
   {0x5D,       0x7D,      0x1D,        ALT_RBRACKET,0  }, /* 221 DD */
   {0,          0,         0x27,        ALT_FQUOTE, 0   }, /* 222 DE */
   {0,          0,         0,           0,          0   }, /* 223 DF VK_OEM_8 */
   {0,          0,         0,           0,          0   }, /* 224 E0 Reserved */
   {0,          0,         0,           0,          0   }, /* 225 E1 OEM-specific */
   {0,          0,         0,           0,          0   }, /* 226 E2 VK_OEM_102 */
   {0,          0,         0,           0,          0   }, /* 227 E3 OEM-specific */
   {0,          0,         0,           0,          0   }, /* 228 E4 OEM-specific */
   {0,          0,         0,           0,          0   }, /* 229 E5 VK_PROCESSKEY */
   {0,          0,         0,           0,          0   }, /* 230 E6 OEM-specific */
   {0,          0,         0,           0,          0   }, /* 231 E7 VK_PACKET */
   {0,          0,         0,           0,          0   }, /* 232 E8 Unassigned */
   {0,          0,         0,           0,          0   }, /* 233 E9 OEM-specific */
   {0,          0,         0,           0,          0   }, /* 234 EA OEM-specific */
   {0,          0,         0,           0,          0   }, /* 235 EB OEM-specific */
   {0,          0,         0,           0,          0   }, /* 236 EC OEM-specific */
   {0,          0,         0,           0,          0   }, /* 237 ED OEM-specific */
   {0,          0,         0,           0,          0   }, /* 238 EE OEM-specific */
   {0,          0,         0,           0,          0   }, /* 239 EF OEM-specific */
   {0,          0,         0,           0,          0   }, /* 240 F0 OEM-specific */
   {0,          0,         0,           0,          0   }, /* 241 F1 OEM-specific */
   {0,          0,         0,           0,          0   }, /* 242 F2 OEM-specific */
   {0,          0,         0,           0,          0   }, /* 243 F3 OEM-specific */
   {0,          0,         0,           0,          0   }, /* 244 F4 OEM-specific */
   {0,          0,         0,           0,          0   }, /* 245 F5 OEM-specific */
   {0,          0,         0,           0,          0   }, /* 246 F6 VK_ATTN */
   {0,          0,         0,           0,          0   }, /* 247 F7 VK_CRSEL */
   {0,          0,         0,           0,          0   }, /* 248 F8 VK_EXSEL */
   {0,          0,         0,           0,          0   }, /* 249 F9 VK_EREOF */
   {0,          0,         0,           0,          0   }, /* 250 FA VK_PLAY */
   {0,          0,         0,           0,          0   }, /* 251 FB VK_ZOOM */
   {0,          0,         0,           0,          0   }, /* 252 FC VK_NONAME */
   {0,          0,         0,           0,          0   }, /* 253 FD VK_PA1 */
   {0,          0,         0,           0,          0   }  /* 254 FE VK_OEM_CLEAR */
};

static const KPTAB ext_kptab[] =
{
   {0,          0,              0,              0,            0}, /*  0  MUST BE EMPTY */
   {PADENTER,   SHF_PADENTER,   CTL_PADENTER,   ALT_PADENTER, 0}, /*  1  13 */
   {PADSLASH,   SHF_PADSLASH,   CTL_PADSLASH,   ALT_PADSLASH, 0}, /*  2 111 */
   {KEY_PPAGE,  KEY_SPREVIOUS,  CTL_PGUP,       ALT_PGUP,     0}, /*  3  33 */
   {KEY_NPAGE,  KEY_SNEXT,      CTL_PGDN,       ALT_PGDN,     0}, /*  4  34 */
   {KEY_END,    KEY_SEND,       CTL_END,        ALT_END,      0}, /*  5  35 */
   {KEY_HOME,   KEY_SHOME,      CTL_HOME,       ALT_HOME,     0}, /*  6  36 */
   {KEY_LEFT,   KEY_SLEFT,      CTL_LEFT,       ALT_LEFT,     0}, /*  7  37 */
   {KEY_UP,     KEY_SUP,        CTL_UP,         ALT_UP,       0}, /*  8  38 */
   {KEY_RIGHT,  KEY_SRIGHT,     CTL_RIGHT,      ALT_RIGHT,    0}, /*  9  39 */
   {KEY_DOWN,   KEY_SDOWN,      CTL_DOWN,       ALT_DOWN,     0}, /* 10  40 */
   {KEY_IC,     KEY_SIC,        CTL_INS,        ALT_INS,      0}, /* 11  45 */
   {KEY_DC,     KEY_SDC,        CTL_DEL,        ALT_DEL,      0}, /* 12  46 */
   {KEY_APPS,   KEY_APPS,       KEY_APPS,       KEY_APPS,     0}, /* 13  93  VK_APPS    */
   {KEY_BROWSER_BACK, KEY_BROWSER_BACK, KEY_BROWSER_BACK, KEY_BROWSER_BACK, 0}, /* 14 166 VK_BROWSER_BACK        */
   {KEY_BROWSER_FWD,  KEY_BROWSER_FWD,  KEY_BROWSER_FWD,  KEY_BROWSER_FWD,  0}, /* 15 167 VK_BROWSER_FORWARD     */
   {KEY_BROWSER_REF,  KEY_BROWSER_REF,  KEY_BROWSER_REF,  KEY_BROWSER_REF,  0}, /* 16 168 VK_BROWSER_REFRESH     */
   {KEY_BROWSER_STOP, KEY_BROWSER_STOP, KEY_BROWSER_STOP, KEY_BROWSER_STOP, 0}, /* 17 169 VK_BROWSER_STOP        */
   {KEY_SEARCH,       KEY_SEARCH,       KEY_SEARCH,       KEY_SEARCH,       0}, /* 18 170 VK_BROWSER_SEARCH      */
   {KEY_FAVORITES,    KEY_FAVORITES,    KEY_FAVORITES,    KEY_FAVORITES,    0}, /* 19 171 VK_BROWSER_FAVORITES   */
   {KEY_BROWSER_HOME, KEY_BROWSER_HOME, KEY_BROWSER_HOME, KEY_BROWSER_HOME, 0}, /* 20 172 VK_BROWSER_HOME        */
   {KEY_VOLUME_MUTE,  KEY_VOLUME_MUTE,  KEY_VOLUME_MUTE,  KEY_VOLUME_MUTE,  0}, /* 21 173 VK_VOLUME_MUTE         */
   {KEY_VOLUME_DOWN,  KEY_VOLUME_DOWN,  KEY_VOLUME_DOWN,  KEY_VOLUME_DOWN,  0}, /* 22 174 VK_VOLUME_DOWN         */
   {KEY_VOLUME_UP,    KEY_VOLUME_UP,    KEY_VOLUME_UP,    KEY_VOLUME_UP,    0}, /* 23 175 VK_VOLUME_UP           */
   {KEY_NEXT_TRACK,   KEY_NEXT_TRACK,   KEY_NEXT_TRACK,   KEY_NEXT_TRACK,   0}, /* 24 176 VK_MEDIA_NEXT_TRACK    */
   {KEY_PREV_TRACK,   KEY_PREV_TRACK,   KEY_PREV_TRACK,   KEY_PREV_TRACK,   0}, /* 25 177 VK_MEDIA_PREV_TRACK    */
   {KEY_MEDIA_STOP,   KEY_MEDIA_STOP,   KEY_MEDIA_STOP,   KEY_MEDIA_STOP,   0}, /* 26 178 VK_MEDIA_STOP          */
   {KEY_PLAY_PAUSE,   KEY_PLAY_PAUSE,   KEY_PLAY_PAUSE,   KEY_PLAY_PAUSE,   0}, /* 27 179 VK_MEDIA_PLAY_PAUSE    */
   {KEY_LAUNCH_MAIL,  KEY_LAUNCH_MAIL,  KEY_LAUNCH_MAIL,  KEY_LAUNCH_MAIL,  0}, /* 28 180 VK_LAUNCH_MAIL         */
   {KEY_MEDIA_SELECT, KEY_MEDIA_SELECT, KEY_MEDIA_SELECT, KEY_MEDIA_SELECT, 0}, /* 29 181 VK_LAUNCH_MEDIA_SELECT */
   {KEY_LAUNCH_APP1,  KEY_LAUNCH_APP1,  KEY_LAUNCH_APP1,  KEY_LAUNCH_APP1,  0}, /* 30 182 VK_LAUNCH_APP1         */
   {KEY_LAUNCH_APP2,  KEY_LAUNCH_APP2,  KEY_LAUNCH_APP2,  KEY_LAUNCH_APP2,  0}, /* 31 183 VK_LAUNCH_APP2         */
};

/* End of kptab[] */

void PDC_set_keyboard_binary(bool on)
{
    DWORD mode;

    GetConsoleMode(pdc_con_in, &mode);
    SetConsoleMode(pdc_con_in, !on ? (mode | ENABLE_PROCESSED_INPUT) :
                                    (mode & ~ENABLE_PROCESSED_INPUT));
}

/* check if a key or mouse event is waiting
 */
bool PDC_check_key(void)
{
    if (key_count > 0)
        return TRUE;

    GetNumberOfConsoleInputEvents(pdc_con_in, &event_count);

    return (event_count != 0);
}

/* _get_key_count returns 0 if save_ip doesn't contain an event which
   should be passed back to the user. This function filters "useless"
   events.

   The function returns the number of keys waiting. This may be > 1
   if the repetition of real keys pressed so far are > 1.

   Returns 0 on NUMLOCK, CAPSLOCK, SCROLLLOCK.

   Returns 1 for SHIFT, ALT, CTRL only if no other key has been pressed
   in between, and SP->return_key_modifiers is set; these are returned
   on keyup.

   Normal keys are returned on keydown only. The number of repetitions
   are returned. Dead keys (diacritics) are omitted. See below for a
   description.
 */
static int _get_key_count(void)
{
    int num_keys = 0, vk;

    vk = KEV.wVirtualKeyCode;

    if (KEV.bKeyDown)
    {
        /* key down */

        save_press = 0;

        if (vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL)
        {
            /* throw away these modifiers */
        }
        else if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
        {
            /* These keys are returned on keyup only. */

            save_press = vk;
            switch (vk)
            {
            case VK_SHIFT:
                left_key = GetKeyState(VK_LSHIFT);
                break;
            case VK_CONTROL:
                left_key = GetKeyState(VK_LCONTROL);
                break;
            case VK_MENU:
                left_key = GetKeyState(VK_LMENU);
            }
        }
        else
        {
            /* Check for diacritics. These are dead keys. Some locales
               have modified characters like umlaut-a, which is an "a"
               with two dots on it. In some locales you have to press a
               special key (the dead key) immediately followed by the
               "a" to get a composed umlaut-a. The special key may have
               a normal meaning with different modifiers.
             */
            if (KEV.uChar.UnicodeChar || !(MapVirtualKey(vk, 2) & 0x80000000))
                num_keys = KEV.wRepeatCount;
        }
    }
    else
    {
        /* key up */

        /* Only modifier keys or the results of ALT-numpad entry are
           returned on keyup */

        if ((vk == VK_MENU && KEV.uChar.UnicodeChar) ||
           ((vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) &&
             vk == save_press))
        {
            save_press = 0;
            num_keys = 1;
        }
    }
    return num_keys;
}

typedef const char *(CDECL *wine_version_func)(void);

static bool running_under_wine( void)
{
    static int rval = -1;

    if( -1 == rval)
    {
         HMODULE hntdll = GetModuleHandleA( "ntdll.dll");

         if( GetProcAddress(hntdll, "wine_get_version") != (FARPROC)NULL)
             rval = 1;
         else
             rval = 0;
    }
    return( (bool)rval);
}

/*
  _process_key_event returns -1 if the key in save_ip should be
  ignored. Otherwise it returns the keycode which should be returned
  by PDC_get_key(). save_ip must be a key event.

  CTRL-ALT support has been disabled, when is it emitted plainly?
 */
static int _process_key_event(void)
{
    int key = KEV.uChar.UnicodeChar;
    WORD vk = KEV.wVirtualKeyCode;
    DWORD state = KEV.dwControlKeyState;

    int idx;
    BOOL enhanced;

    /* Save the key modifiers. Do this first to allow to detect e.g. a
       pressed CTRL key after a hit of NUMLOCK. */

    if (state & (LEFT_ALT_PRESSED|RIGHT_ALT_PRESSED))
        SP->key_modifiers |= PDC_KEY_MODIFIER_ALT;

    if (state & SHIFT_PRESSED)
        SP->key_modifiers |= PDC_KEY_MODIFIER_SHIFT;

    if (state & (LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED))
        SP->key_modifiers |= PDC_KEY_MODIFIER_CONTROL;

    if (state & NUMLOCK_ON)
        SP->key_modifiers |= PDC_KEY_MODIFIER_NUMLOCK;

    /* Handle modifier keys hit by themselves */

    switch (vk)
    {
    case VK_SHIFT: /* shift */
        if (!SP->return_key_modifiers)
            return -1;

        return (left_key & 0x8000) ? KEY_SHIFT_L : KEY_SHIFT_R;

    case VK_CONTROL: /* control */
        if (!SP->return_key_modifiers)
            return -1;

        return (left_key & 0x8000) ? KEY_CONTROL_L : KEY_CONTROL_R;

    case VK_MENU: /* alt */
        if (!key)
        {
            if (!SP->return_key_modifiers)
                return -1;

            return (left_key & 0x8000) ? KEY_ALT_L : KEY_ALT_R;
        }
    }

    /* The system may emit Ascii or Unicode characters depending on
       whether ReadConsoleInputA or ReadConsoleInputW is used.

       Normally, if key != 0 then the system did the translation
       successfully. But this is not true for LEFT_ALT (different to
       RIGHT_ALT). In case of LEFT_ALT we can get key != 0. So
       check for this first. */

    if (key && ( !(state & LEFT_ALT_PRESSED) ||
        (state & RIGHT_ALT_PRESSED) ))
    {
        /* This code should catch all keys returning a printable
           character. Characters above 0x7F should be returned as
           positive codes. */

        if (kptab[vk].extended == 0)
            return key;
    }

    /* This case happens if a functional key has been entered. */

    if ((state & ENHANCED_KEY) && (kptab[vk].extended != 999))
    {
        enhanced = TRUE;
        idx = kptab[vk].extended;
    }
    else
    {
        enhanced = FALSE;
        idx = vk;
    }

    if( idx < 0)
        key = -1;

    else if( enhanced && (size_t)idx >= sizeof( ext_kptab) / sizeof( ext_kptab[0]))
        key = -1;       /* unhandled key outside table */

    else if( !enhanced && (size_t)idx >= sizeof( kptab) / sizeof( kptab[0]))
        key = -1;       /* unhandled key outside table */

    else if (state & SHIFT_PRESSED)
        key = enhanced ? ext_kptab[idx].shift : kptab[idx].shift;

    else if (state & (LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED))
        key = enhanced ? ext_kptab[idx].control : kptab[idx].control;

    else if (state & (LEFT_ALT_PRESSED|RIGHT_ALT_PRESSED))
        key = enhanced ? ext_kptab[idx].alt : kptab[idx].alt;

    else
    {
        key = enhanced ? ext_kptab[idx].normal : kptab[idx].normal;
        if( running_under_wine( ))
        {
            size_t i;          /* Wine mangles some keys. */
            static const int wine_remaps[] = {
                        KEY_A1, KEY_HOME,       KEY_A3, KEY_PPAGE,
                        KEY_C1, KEY_END,        KEY_C3, KEY_NPAGE,
                        KEY_A2, KEY_UP,         KEY_C2, KEY_DOWN,
                        KEY_B3, KEY_RIGHT,      KEY_B1, KEY_LEFT };

            for( i = 0; i < sizeof( wine_remaps) / sizeof( wine_remaps[0]); i += 2)
                if( key == wine_remaps[i])
                    key = wine_remaps[i + 1];
        }
    }

    return key;
}

#define BUTTON_N_CLICKED(N)   PDC_SHIFTED_BUTTON( BUTTON1_CLICKED, (N))

static void _process_mouse_event(void)
{
    int i, event = -1, modifiers = 0;
    const int x = MEV.dwMousePosition.X;
    const int y = MEV.dwMousePosition.Y;

    save_press = 0;

    if (MEV.dwControlKeyState & (LEFT_ALT_PRESSED|RIGHT_ALT_PRESSED))
        modifiers |= BUTTON_ALT;

    if (MEV.dwControlKeyState & (LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED))
        modifiers |= BUTTON_CONTROL;

    if (MEV.dwControlKeyState & SHIFT_PRESSED)
        modifiers |= BUTTON_SHIFT;

    /* Handle scroll wheel */

    if (MEV.dwEventFlags == MOUSE_WHEELED)
        event = (MEV.dwButtonState & 0xFF000000) ?
               PDC_MOUSE_WHEEL_DOWN : PDC_MOUSE_WHEEL_UP;

    if (MEV.dwEventFlags == MOUSE_HWHEELED)
        event = (MEV.dwButtonState & 0xFF000000) ?
               PDC_MOUSE_WHEEL_RIGHT : PDC_MOUSE_WHEEL_LEFT;

    if (MEV.dwEventFlags == 1)
        event = BUTTON_MOVED;

    if( event != -1)
        _add_raw_mouse_event( 0, event, modifiers, x, y);

    if (MEV.dwEventFlags == 0)
    {
        int button = -1;
        static const DWORD button_mask[] = {1, 4, 2};
        static DWORD prev_state;
        DWORD changes = prev_state ^ MEV.dwButtonState;
        bool incomplete_event = FALSE;

        prev_state = MEV.dwButtonState;
        for( i = 0; i < 3; i++)
            if( changes == button_mask[i])
            {
                button = i;
                event = (MEV.dwButtonState & button_mask[button]) ?
                                 BUTTON_PRESSED : BUTTON_RELEASED;
            }

        if( button < 0)
            return;

        incomplete_event = _add_raw_mouse_event( button, event, modifiers, x, y);
        while( incomplete_event)
         {
             DWORD n_mouse_events = 0;
             int remaining_ms = SP->mouse_wait;

             while( !n_mouse_events && remaining_ms)
             {
                 const int nap_len = (remaining_ms > 20 ? 20 : remaining_ms);

                 napms( nap_len);
                 remaining_ms -= nap_len;
                 GetNumberOfConsoleInputEvents(pdc_con_in, &n_mouse_events);
             }
             incomplete_event = FALSE;
             if( n_mouse_events)
             {
                 INPUT_RECORD ip;

                 PeekConsoleInput(pdc_con_in, &ip, 1, &n_mouse_events);
                 if( (prev_state ^ button_mask[button])
                                     == ip.Event.MouseEvent.dwButtonState)
                     {
                     ReadConsoleInput(pdc_con_in, &ip, 1, &n_mouse_events);
                     prev_state ^= button_mask[button];
                     event = (prev_state & button_mask[button]) ?
                                  BUTTON_PRESSED : BUTTON_RELEASED;
                     incomplete_event = _add_raw_mouse_event( button, event, modifiers, x, y);
                     }
             }
        }
    }
}

/* return the next available key or mouse event
 */
int PDC_get_key(void)
{
    if( _get_mouse_event( &SP->mouse_status))
        return( KEY_MOUSE);
    if (!key_count)
    {
        DWORD count;

        ReadConsoleInput(pdc_con_in, &save_ip, 1, &count);
        event_count--;

        if (save_ip.EventType == MOUSE_EVENT ||
            save_ip.EventType == WINDOW_BUFFER_SIZE_EVENT)
            key_count = 1;
        else if (save_ip.EventType == KEY_EVENT)
            key_count = _get_key_count();
    }

    if (key_count)
    {
        key_count--;

        switch (save_ip.EventType)
        {
        case KEY_EVENT:
            SP->key_modifiers = 0L;
            return _process_key_event();

        case MOUSE_EVENT:
            _process_mouse_event();
             if( _get_mouse_event( &SP->mouse_status))
                 return( KEY_MOUSE);
             break;

        case WINDOW_BUFFER_SIZE_EVENT:
            if (REV.dwSize.Y != LINES || REV.dwSize.X != COLS)
            {
                if (!SP->resized)
                {
                    SP->resized = TRUE;
                    return KEY_RESIZE;
                }
            }
        }
    }

    return -1;
}

/* discard any pending keyboard or mouse input -- this is the core
   routine for flushinp()
 */
void PDC_flushinp(void)
{
    FlushConsoleInputBuffer(pdc_con_in);
}

bool PDC_has_mouse(void)
{
    return TRUE;
}

int PDC_mouse_set(void)
{
    DWORD mode;

    /* If turning on mouse input: Set ENABLE_MOUSE_INPUT, and clear
       all other flags, except processed input mode;
       If turning off the mouse: Set QuickEdit Mode to the status it
       had on startup, and clear all other flags, except etc. */

    GetConsoleMode(pdc_con_in, &mode);
    mode = (mode & 1) | 0x0088;
    SetConsoleMode(pdc_con_in, mode | (SP->_trap_mbe ?
                   ENABLE_MOUSE_INPUT : pdc_quick_edit));

    return OK;
}

int PDC_modifiers_set(void)
{
    return OK;
}
