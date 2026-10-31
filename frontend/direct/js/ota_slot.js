/* Firmware upload size on the shared Firmware page (js/pages/ota.js), Direct
   profile only (frontend/direct/build.sh appends this file after it).

   The shared page stops anything over 3 MB before uploading: that is the HA
   app slot. The Direct profile's app slots are 3.5 MB (docs/15 design 2.4,
   partitions_direct.csv) and its image is about 3 MB with the Matter
   controller (M7), so a few more steps of code would push a valid image over
   the shared limit and the page would refuse it. The server checks the real
   slot size (rest_api.c handle_ota_esp32, Direct branch); this only keeps the
   page's early check in line with it. The shared file is not edited: that
   would change the HA www.bin (architecture 5.8). */
OTA_TARGETS.fw.maxSize = 0x380000;
OTA_TARGETS.fw.maxText = '3.5 MB';
OTA_TARGETS.fw.hint = () => t('bitengine-direct-&lt;version&gt;.bin — about 3 MB');
