AzamiOS Persistent Font Storage (/hdd/fonts)
=============================================
You can store custom fonts on this drive!

Supported formats:
  - Native Azami Font (.azf)
  - Linux PC Screen Font (.psf, .psf2, .psfu)

How to use:
  1. Copy your .azf or .psf font file to /hdd/fonts/
  2. List available fonts:
     setfont -l
  3. Preview the font:
     setfont -p /hdd/fonts/custom_sample.azf
  4. Set as system default font:
     setfont /hdd/fonts/custom_sample.azf
