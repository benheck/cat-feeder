# GPIO Compatibility Guide - Pi4 vs Pi5

Great news! Both your Raspberry Pi 4 and Pi 5 are running the same libgpiod version (v2.2.1), which means **no conditional compilation is needed**. The same code works on both platforms!

## Development vs Deployment

- **Development**: Done on Pi5 for better VS Code performance (more RAM, faster CPU)
- **Deployment**: Can be deployed to either Pi4 or Pi5 using the same build process

## Build Instructions

### For Both Pi4 and Pi5
```bash
mkdir -p build
cd build
cmake ..
make
```

**Simple**: No special flags needed! The same build works on both platforms since they use the same GPIO library version.

## Deployment Workflow

1. **Develop on Pi5**: Use the standard build for testing and development
2. **Deploy to Pi4**: Transfer source code to Pi4 and build with standard `cmake ..`
3. **Deploy to Pi5**: Use the same standard build process

## Library Compatibility

### Both Pi4 and Pi5
- libgpiod v2.2.1 (same version on both platforms)
- Build command: `cmake .. && make`
- Uses: `gpiod_chip_open()`, `gpiod_line_request*` pointers
- GPIO API: libgpiod v2.x throughout

## Hardware Wiring

Both Pi4 and Pi5 use the same GPIO pin assignments:
- Up Button: GPIO 17
- Down Button: GPIO 27
- Select Button: GPIO 22
- Cancel Button: GPIO 23

All buttons use internal pull-up resistors, so wire buttons between GPIO and GND.

## Prerequisites

### On Pi4
```bash
sudo apt update
sudo apt install libgpiod-dev build-essential cmake libi2c-dev
```

### On Pi5  
```bash
sudo apt update
sudo apt install libgpiod-dev build-essential cmake libi2c-dev
```

## Cross-Platform Development Tips

1. **Use Pi5 for development** - Better performance for VS Code and compilation
2. **Same build everywhere** - No special flags or conditional compilation needed
3. **Version control** - Use git to sync source code between Pi4 and Pi5
4. **Unified codebase** - One version of the code works on both platforms

## Why This Works

Both your Pi4 and Pi5 systems have:
- **Same OS version** - Modern Raspberry Pi OS
- **Same libgpiod version** - v2.2.1 on both platforms
- **Same GPIO API** - libgpiod v2.x throughout
- **Same build tools** - cmake, gcc, etc.

This eliminates the complexity of conditional compilation and makes deployment much simpler!

## Cost Consideration

Pi4 is more cost-effective for embedded projects like cat feeders, while Pi5 provides better development experience with more RAM and faster compilation.