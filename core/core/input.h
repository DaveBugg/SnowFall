// Pointer input forwarded to the simulation while interaction mode is active.
#pragma once

namespace snow {

struct InputEvent {
    enum class Type {
        PointerMove,   // cursor moved (sent whether or not the button is held)
        PointerDown,   // left button pressed on a hit region (the press was captured)
        PointerUp,     // left button released after a captured press
        CycleTool,     // switch Mitten -> Shovel -> Snowball -> Mitten
    };
    Type type = Type::PointerMove;
    float x = 0, y = 0;   // physical px, virtual desktop
};

}  // namespace snow
