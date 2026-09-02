# Driver behavioural fingerprint catalogue
#
# One .txt per (driver, version, controller): identity, dialects, and the
# sorted pin list from a devsoak classification run. Regenerate any file
# with ci/fingerprint.sh. Pins are behavioural — two correct drivers can
# differ; a changed pin on re-run is a behavioural regression.
#
file                                     version   result    identity
a4091.device-42.39-openrom.txt           42.39     PASS      a4091 42.39 (30.7.2026)
copperhf.device-1.0.txt                  1.0       PASS      copperhf.device  [HD_SCSICMD autosense: SCSICmd sense-offset bug]
lide.device-40.12-ripple.txt             40.12     PASS      lide 40.12 (03.08.2026) Release-40.12-a387f65
scsi.device-37.64-a2091.txt              37.64     PASS      SCSI/XT driver
scsi.device-45.7-a3000-sdmac.txt         45.7      PASS      scsidisk 45.7 (16.5.2018)
scsi.device-47.4-a1200-ide.txt           47.4      PASS      IDE_scsidisk 47.4 (30.12.2019)
scsi.device-47.4-a3000-sdmac.txt         47.4      PASS      scsidisk 47.4 (30.12.2019)
trackdisk.device-34.1-ks13.txt           34.1      PASS      (pre-NSD, no idstring)
trackdisk.device-40.1-ks31.txt           40.1      PASS      (pre-NSD, no idstring)
trackdisk.device-47.14-ks32.txt          47.14     FAIL rc=10 (pre-NSD, no idstring)

## scsi.device pin drift across OS versions (- = test not reached):
pin                  V37/a2091   V45/a3000   V47/ide     V47/a3000  
past-end-err        —           20          3           20          
unaligned-len-err   —           -4          -4          -4          
drivetype-err       -1          -3          -3          -3          
bad-unit-err        50          50          50          50          
flush-err           0           0           0           0           
