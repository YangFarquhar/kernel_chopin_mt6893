# Chopin144 Experimental V0.3

Target: POCO X3 GT / chopin / MT6893 / K10A 42-02-0B DSC VDO.

This is a first runtime integration build. It appends a seventh DRM mode and
wraps the MTK panel-extension callbacks. Candidate profile:

- 1080x2400 active
- HFP 72, HSA 10, HBP 8
- VFP 30, VSA 2, VBP 18
- pixel clock 412776 kHz (1170 x 2450 totals)
- PLL 570, DSI data rate 1140
- DCS selector 0x1F

The panel driver documents 0x22/0x21/0x20 as its existing 60.5/91.2/121.1Hz
selections. The 0x1F 144Hz selector and host profile are experimental candidates,
not vendor-confirmed values. Logs:
- Kernel log tag: chopin144_experimental
- Service log: /data/adb/chopin144_experimental_v0.3.log

This is an experimental test build, not a validated production module.
