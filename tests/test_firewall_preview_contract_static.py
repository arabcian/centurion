#!/usr/bin/env python3
"""Source-level protocol guards. Native Rust/Qt tests are separate."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
backend=(root/'crates/centurion-helpers/src/netguard.rs').read_text()
handler=backend.split('pub fn handle(req:',1)[1]
assert handler.index('helper_lock("netguard-helper")') < handler.index('expected_firewall_matches(req, &cfg.fw)') < handler.index('cfg.fw.apply(req)') < handler.index('nft_feed(&["-c"')
ui=(root/'gui/src/netfirewall.cpp').read_text()
submit=ui.split('void FirewallTab::submitChange',1)[1].split('void FirewallTab::policyChosen',1)[0]
assert 'expected != fw_' in submit and 'canonical.remove(QStringLiteral("shadowed"))' in submit and 'expected_firewall' in submit
preview=ui.split('void FirewallTab::previewConnection',1)[1]
assert 'firewallConnectionPreview' in preview and 'changeRequested' not in preview and 'submitChange' not in preview and 'inspectRequested' not in preview
assert 'config_error' in preview
page=(root/'gui/src/netpage.cpp').read_text()
assert 'RecordedConnectionRole' in page and 'firewall_->previewConnection(recorded)' in page
print('PASS: saved-chain preview and locked expected snapshot source contracts (not runtime tests)')
