#include "Utils.h"

int Utils::getWrappedLines(String (&lines)[MAX_WRAPPED_LINES], String str, int limit) {
    char buf[str.length() + 1];
    char lineBuf[limit + 1];
    str.toCharArray(buf, str.length() + 1);

    char *p = buf;
    char *eol;
    int lineCount = 0;
    for (int i = 0; i < MAX_WRAPPED_LINES; i++) {
        if (p - buf > strlen(buf)) {
            lines[i] = "";
            continue;
        }
        eol = strchr(p, '\n');
        if (eol == NULL) {
            eol = p + strlen(p);
        }

        if (eol - p > limit) {
            eol = p + limit;
            while (*eol != ' ' && *eol != '\n' && eol > p) {
                eol--;
            }
        }
        strncpy(lineBuf, p, eol - p);
        lineBuf[eol - p] = '\0';

        lines[i] = String(lineBuf);
        lineCount++;
        p = eol + 1;
    }
    return lineCount;
}

String Utils::getWrappedLine(String str, int limit, int lineNum, int maxLines) {
    if (lineNum > maxLines) {
        return "";
    }
    char buf[str.length() + 1];
    char lineBuf[limit + 1];
    str.toCharArray(buf, str.length() + 1);

    String lines[maxLines];

    char *p = buf;
    char *eol;

    for (int i = 0; i < maxLines && i <= lineNum; i++) {
        if (p - buf > strlen(buf)) {
            lines[i] = "";
            continue;
        }
        eol = strchr(p, '\n');
        if (eol == NULL) {
            eol = p + strlen(p);
        }

        if (eol - p > limit) {
            eol = p + limit;
            while (*eol != ' ' && *eol != '\n' && eol > p) {
                eol--;
            }
        }
        strncpy(lineBuf, p, eol - p);
        lineBuf[eol - p] = '\0';

        lines[i] = String(lineBuf);
        p = eol + 1;
    }
    return lines[lineNum];
}

int32_t Utils::stringToColor(String color) {
    uint16_t result;
    if (!tryStringToColor(color, result)) {
        Serial.print("Invalid color: ");
        Serial.println(color);
        return TFT_BLACK;
    }
    return result;
}

// Same names as stringToColor(), but reports an unknown one instead of falling back to black - for
// callers that need to reject bad input rather than draw something the sender didn't ask for.
bool Utils::tryStringToColor(String color, uint16_t &out) {
    color.toLowerCase();
    color.replace(" ", "");
    if (color == "black") {
        out = TFT_BLACK;
    } else if (color == "navy") {
        out = TFT_NAVY;
    } else if (color == "darkgreen") {
        out = TFT_DARKGREEN;
    } else if (color == "darkcyan") {
        out = TFT_DARKCYAN;
    } else if (color == "maroon") {
        out = TFT_MAROON;
    } else if (color == "purple") {
        out = TFT_PURPLE;
    } else if (color == "olive") {
        out = TFT_OLIVE;
    } else if (color == "lightgrey" || color == "grey") {
        out = TFT_LIGHTGREY;
    } else if (color == "darkgrey") {
        out = TFT_DARKGREY;
    } else if (color == "blue") {
        out = TFT_BLUE;
    } else if (color == "green") {
        out = TFT_GREEN;
    } else if (color == "cyan") {
        out = TFT_CYAN;
    } else if (color == "red") {
        out = TFT_RED;
    } else if (color == "magenta") {
        out = TFT_MAGENTA;
    } else if (color == "yellow") {
        out = TFT_YELLOW;
    } else if (color == "white") {
        out = TFT_WHITE;
    } else if (color == "orange") {
        out = TFT_ORANGE;
    } else if (color == "greenyellow") {
        out = TFT_GREENYELLOW;
    } else if (color == "pink") {
        out = TFT_PINK;
    } else if (color == "brown") {
        out = TFT_BROWN;
    } else if (color == "gold") {
        out = TFT_GOLD;
    } else if (color == "silver") {
        out = TFT_SILVER;
    } else if (color == "skyblue") {
        out = TFT_SKYBLUE;
    } else if (color == "vilolet") {
        out = TFT_VIOLET;
    } else {
        return false;
    }
    return true;
}

String Utils::formatFloat(float value, int8_t digits) {
    char tmp[30] = {};
    dtostrf(value, 1, digits, tmp);
    return tmp;
}

int32_t Utils::stringToAlignment(String alignment) {
    alignment.toLowerCase();
    if (alignment.indexOf(" ") != -1) {
        alignment = alignment[0] + alignment.substring(alignment.indexOf(" ") + 1, 1);
    }
    alignment.replace(" ", "");
    if (alignment == "tl") {
        return TL_DATUM;
    } else if (alignment == "tc") {
        return TC_DATUM;
    } else if (alignment == "tr") {
        return TR_DATUM;
    } else if (alignment == "ml") {
        return ML_DATUM;
    } else if (alignment == "mc") {
        return MC_DATUM;
    } else if (alignment == "mr") {
        return MR_DATUM;
    } else if (alignment == "bl") {
        return BL_DATUM;
    } else if (alignment == "bc") {
        return BC_DATUM;
    } else if (alignment == "br") {
        return BR_DATUM;
    } else if (alignment == "cl") {
        return CL_DATUM;
    } else if (alignment == "cc") {
        return CC_DATUM;
    } else if (alignment == "cr") {
        return CR_DATUM;
    } else if (alignment == "bl") {
        return BL_DATUM;
    } else if (alignment == "bc") {
        return BC_DATUM;
    } else if (alignment == "br") {
        return BR_DATUM;
    } else if (alignment == "l") {
        return L_BASELINE;
    } else if (alignment == "c") {
        return C_BASELINE;
    } else if (alignment == "r") {
        return R_BASELINE;
    } else {
        return TL_DATUM;
    }
}

uint16_t Utils::rgb565dim(uint16_t color, uint8_t brightness, bool swapBytes) {
    if (color == TFT_BLACK or brightness == 0) {
        return 0;
    }
    if (swapBytes) {
        // swap bytes
        color = (color >> 8) | (color << 8);
    }

    // Extract 5-bit Red, 6-bit Green, and 5-bit Blue components
    uint8_t r5 = (color >> 11) & 0x1F;
    uint8_t g6 = (color >> 5) & 0x3F;
    uint8_t b5 = color & 0x1F;

    // Scale brightness (0-255) to (0-32) and (0-64) ranges
    uint16_t r5_dim = (r5 * brightness + 127) / 255; // Round by adding 127
    uint16_t g6_dim = (g6 * brightness + 127) / 255;
    uint16_t b5_dim = (b5 * brightness + 127) / 255;

    // Recombine into RGB565 format
    uint16_t result = (r5_dim << 11) | (g6_dim << 5) | b5_dim;

    if (swapBytes) {
        // swap bytes
        result = (result >> 8) | (result << 8);
    }
    return result;
}
