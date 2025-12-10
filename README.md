# WellCom Firmware  
**ESP32-C3 Wellness Communication Device**

This repository contains the firmware for the WellCom device, a wellness‑check communicator that sends “Well,” “Ill,” and automated reminder messages through a secure backend.

---

## 📌 Project Description  
WellCom is a small ESP32‑C3 device designed to let older adults send quick wellness notifications to loved ones.  
Communication happens through:

- A **Flask backend** running on **Heroku**  
- SMS delivery handled by **Twilio** (backend only — secrets never reside on the device)  
- Wi-Fi setup through a built‑in **AP configuration portal**

---

## 📦 Firmware Architecture Overview  

### 🔹 Hardware  
- **Seeed XIAO ESP32‑C3**  
- LEDs: Green (GPIO4 / D2), Red (GPIO5 / D3)  
- Buttons:  
  - WELL  (GPIO6 / D4)  
  - ILL   (GPIO7 / D5)  
  - RESET (GPIO21 / D6)  

---

## 🔹 Persistent Settings Stored in NVS  
- Wi-Fi SSID / Password  
- To / From phone numbers  
- To / From names  
- Timezone selection + custom UTC offset  
- Device name (set once via initialization sketch)  
- Last Well/Ill day  
- Last None‑reminder day  

---

## 🔹 Message Types  
| Type | Sent To | Purpose |
|------|---------|---------|
| **Test** | From-phone | Device boot confirmation |
| **Well** | To-phone | User is doing well |
| **Ill** | To-phone | User is *not* feeling well |
| **None** | Both numbers | Automatic reminder after 10 AM |

All messages include:  
- Local time  
- Device name  
- Firmware version  
- Human-friendly message text  

---

## 🔹 Backend Interaction  
The device contacts:

```
https://wellcom-backend-XXXXX.herokuapp.com/api/v1/send_sms
```

It sends a JSON body containing:

```json
{
  "device_name": "...",
  "firmware": "1.1.x.x",
  "to": "+1XXXXXXXXXX",
  "from": "+1XXXXXXXXXX",
  "message": "...."
}
```

The backend validates fields and (eventually) sends SMS via Twilio.

---

## 🔹 OTA Updates  
Triggered by **holding RESET for >10 seconds**.  
Firmware is downloaded from:

```
OTA_FIRMWARE_URL = "https://example.com/WellCom_x.x.x.bin"
```

---

## 🔹 Configuration Portal  
Activated by **holding RESET for 5–10 seconds**.  
Device creates open AP:

```
SSID: WellCom
IP:   192.168.4.1
```

User enters:

- Wi-Fi credentials  
- Phone numbers  
- Names  
- Timezone  

---

## 📁 Files in This Repository  

| File | Purpose |
|------|---------|
| `WellCom_Firmware.ino` | Main firmware source |
| `WellCom_v1.x.x.bin` | Exported binary firmware for OTA updates |
| `README.md` | This file |

---

## 🚀 How to Use This Repository  
1. Clone or download the repo  
2. Open firmware in Arduino IDE (ESP32 board package installed)  
3. Flash to Seeed XIAO ESP32‑C3  
4. Run “initialization sketch” to set device name  
5. Use AP-mode config page to enter Wi-Fi + phone info  
6. Test messages will appear automatically on boot  

---

## 🛠 Future Enhancements  
- Backend Twilio integration (with secure Heroku config vars)  
- Database of registered devices  
- Web dashboard for subscription + usage  
- Optional phone-app companion  

---

## 📄 License  
This project belongs to its author and is not currently licensed for distribution.  
Contact developer for permissions.

---

*Generated README for the WellCom Firmware Project.*