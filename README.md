# Web RC Car 🚗💨

This project transforms a standard radio-controlled car into a fully interactive, web-controlled vehicle with real-time video streaming. It allows you to drive the car from anywhere using a web browser, receiving a low-latency video feed directly from the car's perspective.

This repository contains the core C++ application that runs on the Raspberry Pi, managing hardware control, video processing, and WebRTC communication.

The web client for controlling the car is in a separate repository: **[web_rc_car_client](https://github.com/mpavk/web_rc_car_client)**.

---

### 🚀 Key Features

* **Real-Time Video Streaming:** Low-latency, high-framerate video feed directly in your browser using **WebRTC**.
* **Browser-Based Control:** Receives movement commands (forward, backward, left, right) from the web client.
* **Embedded Intelligence:** Runs on a **Raspberry Pi**, handling all logic on board.
* **Efficient Video Pipeline:** Utilizes **GStreamer** for capturing video from the camera and piping it into the WebRTC stack for optimal performance.
* **High-Performance Core:** The control logic and WebRTC data channel are written in **C++** for maximum performance and reliability.
* **Autostart on Boot:** Configured to launch automatically when the Raspberry Pi is powered on.

---

### 🛠️ Architecture Overview

The system consists of three main components:

1.  **The Car (This Repository):** A Raspberry Pi connected to the car's motor controller and a camera. It runs this C++ application, which:
    * Captures video using GStreamer.
    * Establishes a WebRTC peer connection to a browser.
    * Streams video and receives control commands over a data channel.
    * Translates commands into GPIO signals to control the motors.

2.  **Signaling Server:** A lightweight Node.js server (part of the client repository) to facilitate the initial WebRTC handshake between the car and the browser.

3.  **The Browser (Controller):** A web page that establishes a peer-to-peer connection with the car to send commands and display the video feed.

---

### ⚙️ Hardware Requirements

* **Raspberry Pi:** Model 3B+ or newer recommended.
* **RC Car Chassis:** A simple 2-wheel drive chassis.
* **Motor Driver:** L298N or a similar H-bridge motor driver.
* **Camera:** Raspberry Pi Camera Module.
* **Power:** A portable power source (e.g., a power bank) for the Raspberry Pi and a separate battery pack for the motors.

---

### 📋 Installation & Setup on Raspberry Pi

1.  **Clone the Repository:**
    ```bash
    git clone https://github.com/mpavk/web_rc_car.git
    cd web_rc_car
    ```

2.  **Install Dependencies:**
    You will need to install `GStreamer`, `libwebrtc`, and other development libraries.
    ```bash
    # Update package list
    sudo apt-get update

    # Install GStreamer and build tools
    sudo apt-get install -y build-essential cmake libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev

    # Note: libwebrtc installation can be complex.
    # You may need to build it from source or find a pre-compiled version for ARM.

    ```

3.  **Build the Project:**
    Use CMake to build the C++ application.
    ```bash
    mkdir build
    cd build
    cmake ..
    make
    ```
    This will create an executable file named `webrccar` in the `build` directory.

---

### 🚀 Usage

The application requires command-line arguments to specify the signaling server's address and the client ID.

#### Command-Line Arguments

The executable accepts the following arguments:
1.  **Signaling Server IP:** The public IP address of the machine running the signaling server.
2.  **Signaling Server Port:** The port the signaling server is listening on (e.g., `8443`).
3.  **Client ID:** A unique identifier for the car client (e.g., `vid`).

#### How to Run

1.  **Start the Signaling Server**: First, ensure the [signaling server](https://github.com/mpavk/web_rc_car_client) is running and accessible at a public IP address.

2.  **Run the Car Application**: Launch the compiled executable from the `build` directory, passing the required arguments.

    ```bash
    ./build/webrccar <SERVER_IP> <PORT> <CLIENT_ID>
    ```

    **Example:**
    ```bash
    ./build/webrccar 83.171.133.2 8443 vid
    ```

3.  **Connect from the Browser**: Open the web client and use the same Client ID (`vid` in this example) to connect to the car.

#### Autostart on Boot

The `start_all.sh` script should be updated to include these arguments.

**Example `start_all.sh`:**
```bash
#!/bin/bash
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd)

# Launch the main application with the required arguments
"$SCRIPT_DIR/build/webrccar" 83.171.133.2 8443 vid
