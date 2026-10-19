#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Inspect the pinned VMOS display card layout and its DEX aspect-ratio setup."""
import argparse
import json
from pathlib import Path
import re
import zipfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('apk',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
from loguru import logger
logger.remove()
from androguard.core.apk import APK
from androguard.core.axml import AXMLPrinter
from androguard.core.dex import DEX
from lxml import etree
app=APK(str(a.apk));a.output.mkdir(parents=True,exist_ok=True)
# FragmentVmBinding.d uses w.f.o in the inspected 2.0.0-alpha09 artifact.
layouts=app.get_android_resources().get_resolved_res_configs(0x7f0b003d)
assert len(layouts)==1,layouts
path=layouts[0][1];xml=AXMLPrinter(app.get_file(path)).get_xml(pretty=True)
root=etree.fromstring(xml);ns='{http://schemas.android.com/apk/res/android}'
card=root.find('.//androidx.cardview.widget.CardView');assert card is not None
surface=card.find('com.vmos.virtual.ui.view.AppVMOSTextureView');assert surface is not None
checks={
 'surface_matches_card_width':surface.get(ns+'layout_width')=='-1',
 'surface_matches_card_height':surface.get(ns+'layout_height')=='-1',
 'card_has_constraint_sized_width':card.get(ns+'layout_width')=='0.000000dip',
 'card_has_constraint_sized_height':card.get(ns+'layout_height')=='0.000000dip',
 'no_spinner_in_display_layout':not root.findall('.//Spinner') and not root.findall('.//android.widget.Spinner'),
 'no_text_buttons_in_display_layout':not root.findall('.//Button') and not root.findall('.//android.widget.Button'),
}
listing=None
with zipfile.ZipFile(a.apk) as z:
 for name in z.namelist():
  if not re.fullmatch(r'classes\d*\.dex',name):continue
  for cls in DEX(z.read(name)).get_classes():
   if cls.get_name()!='Lcom/vmos/virtual/ui/fragments/VmFragment;':continue
   method=next(m for m in cls.get_methods() if m.get_name()=='onViewCreated')
   listing='\n'.join(i.get_name()+' '+i.get_output() for i in method.get_instructions())
assert listing is not None
for needle in ('dimensionRatio','getWidth','getHeight'):
 checks['dex_'+needle]=needle in listing
(a.output/'fragment-vm.xml').write_bytes(xml)
(a.output/'display-page-dex.txt').write_text(listing+'\n')
report={'classification':'Binary XML and DEX derived; static only','target_executed':False,
        'package':app.get_package(),'version':app.get_androidversion_name(),'layout_path':path,'checks':checks}
(a.output/'display-page-checks.json').write_text(json.dumps(report,indent=2)+'\n')
assert all(checks.values()),checks
print('PASS: '+str(len(checks))+' display-card layout/aspect-ratio checks; target not executed')
