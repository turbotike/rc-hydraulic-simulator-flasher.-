
const TABS = [
  { id:'machine',   label:'Machine',          icon:'\u2699' },
  { id:'sounds',    label:'Sounds',            icon:'\u266B' },
  { id:'rc',        label:'RC Input',          icon:'\u25CE' },
  { id:'esc',       label:'ESC / Drive',       icon:'\u26A1' },
  { id:'servos',    label:'Channels',          icon:'\u21C4' },
  { id:'soundtech', label:'Sound Technician',  icon:'\u2692' },
  { id:'build',     label:'Build Log',         icon:'\u25B6' },
];

// Machine-specific channel labels
const CHANNEL_LABELS = {
  'EXCAVATOR_MODE': ['Bucket', 'Swing', 'Throttle', 'Horn', 'Boom', 'Stick', 'Track L', 'Track R'],
  'LOADER_MODE':    ['Bucket', 'Boom', 'Throttle', 'Horn', 'Eng On/Off', 'Aux', 'Steer', 'Drive'],
  'CRANE_MODE':     ['Boom Lift', 'Boom Ext', 'Throttle', 'Horn', 'Eng On/Off', 'Rope', 'Aux', 'Swing'],
  'DOZER_MODE':     ['Blade', 'Ripper', 'Throttle', 'Horn', 'Eng On/Off', 'Aux', 'Tilt', 'Track'],
  'SKIDSTEER_MODE': ['Bucket', 'Boom', 'Throttle', 'Horn', 'Eng On/Off', 'Hi/Lo', 'Drive L', 'Drive R'],
  'GRADER_MODE':    ['Blade Lift', 'Circle', 'Throttle', 'Horn', 'Eng On/Off', 'Hi/Lo', 'Tilt', 'Articulation'],
};

// Channel mapping: which config variable maps to which function per machine type
// { varName: 'CH_XX_YYY', label: 'Function Name' }
const CH_MAP_COMMON = [
  { v: 'CH_THROTTLE',      label: 'Throttle' },
  { v: 'CH_HORN',          label: 'Horn' },
  { v: 'CH_ENGINE_TOGGLE', label: 'Engine On/Off' },
  { v: 'CH_HILO_TOGGLE',   label: 'Hi/Lo Toggle' },
  { v: 'CH_LIGHTS',        label: 'Lights' },
];
const CH_MAP_MACHINE = {
  'EXCAVATOR_MODE': [
    { v: 'CH_EX_TRACK_L', label: 'Track L' },
    { v: 'CH_EX_TRACK_R', label: 'Track R' },
    { v: 'CH_EX_BOOM',    label: 'Boom' },
    { v: 'CH_EX_STICK',   label: 'Arm' },
    { v: 'CH_EX_BUCKET',  label: 'Bucket' },
    { v: 'CH_EX_SWING',   label: 'Swing' },
  ],
  'LOADER_MODE': [
    { v: 'CH_LD_BUCKET', label: 'Bucket' },
    { v: 'CH_LD_BOOM',   label: 'Boom' },
  ],
  'CRANE_MODE': [
    { v: 'CH_CR_BOOM',   label: 'Boom Lift' },
    { v: 'CH_CR_EXTEND', label: 'Boom Extend' },
    { v: 'CH_CR_SWING',  label: 'Swing' },
  ],
  'DOZER_MODE': [
    { v: 'CH_DZ_BLADE',  label: 'Blade Lift' },
    { v: 'CH_DZ_TILT',   label: 'Blade Tilt' },
    { v: 'CH_DZ_RIPPER', label: 'Ripper' },
  ],
  'SKIDSTEER_MODE': [
    { v: 'CH_SS_BUCKET', label: 'Bucket' },
    { v: 'CH_SS_BOOM',   label: 'Boom' },
  ],
  'GRADER_MODE': [
    { v: 'CH_GR_BLADE',        label: 'Blade Lift' },
    { v: 'CH_GR_CIRCLE',       label: 'Circle Rotation' },
    { v: 'CH_GR_TILT',         label: 'Blade Tilt' },
    { v: 'CH_GR_ARTICULATION', label: 'Articulation' },
  ],
};

const MACHINE_NAMES = {
  'EXCAVATOR_MODE': 'Excavator',
  'LOADER_MODE': 'Wheel Loader',
  'CRANE_MODE': 'Crane',
  'DOZER_MODE': 'Dozer',
  'SKIDSTEER_MODE': 'Skid Steer',
  'GRADER_MODE': 'Motor Grader',
};

let CFG = {};
let soundFiles = [];

async function init() {
  try {
  const resp = await fetch('/api/config');
  const data = await resp.json();
  soundFiles = data.soundFiles || [];
  delete data.soundFiles;
  CFG = data;
  renderTabs();
  renderPanels();
  updateMachineName();
  activateTab('machine');
  } catch(e) { document.body.innerHTML = '<pre style="color:red;padding:20px;">INIT ERROR: ' + e.message + '\n\n' + e.stack + '</pre>'; }
}

function updateMachineName() {
  const el = document.getElementById('machineName');
  if (el) el.textContent = CFG.customMachineName || MACHINE_NAMES[CFG.machineType] || 'Unknown';
}

function renderTabs() {
  const sb = document.getElementById('sidebar');
  sb.innerHTML =
    '<div class="tree-header">\u25BC Available Modules</div>' +
    TABS.map(t =>
      `<div class="tree-node" data-tab="${t.id}" onclick="activateTab('${t.id}')">` +
      `<span class="tree-icon">${t.icon}</span>${t.label}</div>`
    ).join('') +
    '<div class="tree-status" id="treeStatus">Ready</div>';
}

function activateTab(id) {
  document.querySelectorAll('.tree-node').forEach(n => n.classList.toggle('active', n.dataset.tab === id));
  document.querySelectorAll('.panel').forEach(p => p.classList.toggle('active', p.id === 'p-'+id));
  if (id === 'build') pollBuild();
  if (id === 'soundtech') { stLoadBrowser(); if (typeof refreshSndpackPorts === 'function') refreshSndpackPorts(); }
  if (id === 'servos' && typeof refreshPorts === 'function') refreshPorts();
  const st = document.getElementById('treeStatus');
  if (st) { const t = TABS.find(t=>t.id===id); st.textContent = t ? t.label : id; }
}

function soundSelect(slot, current) {
  const opts = soundFiles.map(f =>
    `<option value="${f}" ${f===current?'selected':''}>${f.replace('.h','')}</option>`
  ).join('');
  return `<select data-sound="${slot}">${opts}</select>
    <button class="btn btn-ghost btn-sm" onclick="stPreviewCurrent('${slot}')" title="Preview sound" style="margin-left:4px;">&#9654;</button>
    <button class="btn btn-ghost btn-sm" onclick="sbStop()" title="Stop" style="margin-left:2px;">&#9632;</button>`;
}

function numField(label, key, min, max, step) {
  const v = CFG[key] ?? 0; step = step || 1;
  return `<div class="field">
    <label>${label}</label>
    <input type="range" min="${min}" max="${max}" step="${step}" value="${v}"
      oninput="CFG['${key}']=+this.value; this.nextElementSibling.textContent=this.value">
    <span class="range-val">${v}</span>
  </div>`;
}

function numInput(label, key, min, max) {
  const v = CFG[key] ?? 0;
  return `<div class="field">
    <label>${label}</label>
    <input type="number" min="${min}" max="${max}" value="${v}"
      onchange="CFG['${key}']=+this.value">
  </div>`;
}

function radioGroup(key, options, onChange) {
  const extra = onChange ? ` ${onChange}` : '';
  return `<div class="radio-group">${options.map(o =>
    `<label><input type="radio" name="${key}" value="${o.value}" ${CFG[key]===o.value?'checked':''}
      onchange="CFG['${key}']=this.value;${extra}"> ${o.label}</label>`
  ).join('')}</div>`;
}

function checkbox(label, key) {
  const v = CFG[key];
  return `<label class="sw"><input type="checkbox" ${v?'checked':''}
    onchange="CFG['${key}']=this.checked"><span class="sl"></span></label> ${label}`;
}

function getChannelLabels() {
  // Build labels for CH1-CH16 from actual CH_* config values
  const labels = [];
  for (let i = 0; i < 16; i++) labels.push('');
  const all = [...CH_MAP_COMMON, ...(CH_MAP_MACHINE[CFG.machineType] || [])];
  for (const m of all) {
    const ch = CFG[m.v];
    if (ch >= 1 && ch <= 16) labels[ch - 1] = m.label;
  }
  return labels;
}

function liveSlider(key, label, min, max, unit) {
  const val = CFG[key] || min;
  return `<div style="display:flex;align-items:center;gap:6px;margin-bottom:4px;">
    <span style="min-width:100px;font-size:11px;color:var(--dim);">${label}</span>
    <input type="range" min="${min}" max="${max}" value="${val}" style="flex:1;accent-color:var(--accent);"
      oninput="CFG['${key}']=+this.value;this.nextElementSibling.textContent=this.value+'${unit}'">
    <span style="min-width:45px;font-size:11px;color:var(--text);text-align:right;">${val}${unit}</span>
  </div>`;
}

function liveToggle(key, label, hint) {
  const v = (CFG[key] === 'true' || CFG[key] === true);
  return `<div style="display:flex;align-items:center;gap:8px;margin:6px 0;">
    <label class="sw"><input type="checkbox" ${v?'checked':''}
      onchange="CFG['${key}']=this.checked?'true':'false'"><span class="sl"></span></label>
    <span style="font-size:11px;color:var(--dim);">${label}${hint ? ' â€” <em>'+hint+'</em>' : ''}</span>
  </div>`;
}

function renderPanels() {
  document.getElementById('panels').innerHTML = `
    ${panelMachine()}
    ${panelSounds()}
    ${panelRC()}
    ${panelESC()}
    ${panelServos()}
    ${panelSoundTech()}
    ${panelBuild()}
  `;
}

function onMachineChange() {
  updateMachineName();
  // Re-render the entire Channels panel (labels + mapping change per machine)
  const servoPanel = document.getElementById('p-servos');
  if (servoPanel) servoPanel.outerHTML = panelServos();
}

function panelMachine() {
  return `<div class="panel" id="p-machine">
    <div class="section-title">Machine Type</div>
    ${radioGroup('machineType', [
      {value:'EXCAVATOR_MODE', label:'Excavator'},
      {value:'LOADER_MODE', label:'Wheel Loader'},
      {value:'CRANE_MODE', label:'Crane'},
      {value:'DOZER_MODE', label:'Dozer'},
      {value:'SKIDSTEER_MODE', label:'Skid Steer'},
      {value:'GRADER_MODE', label:'Motor Grader'},
    ], 'onMachineChange()')}
    <div class="section-title">Custom Machine Name</div>
    <div class="field">
      <label>Display Name</label>
      <input type="text" id="customMachineName" value="${CFG.customMachineName||''}"
        style="background:var(--surface);border:1px solid var(--border);color:var(--text);padding:6px 10px;border-radius:6px;font-size:13px;font-family:var(--font);width:280px;"
        oninput="CFG.customMachineName=this.value;updateMachineName()"
        placeholder="${MACHINE_NAMES[CFG.machineType]||''}">
    </div>
    <p class="hint">Override the display name shown in the top bar (leave blank for default)</p>
    <div class="section-title">Master Volume</div>
    ${numField('Master Volume %', 'masterVolume', 0, 300, 5)}
    <div class="section-title">Debug Output</div>
    <div class="check-group">
      ${checkbox('RC Channels', 'debugRc')}
      ${checkbox('ESC State', 'debugEsc')}
      ${checkbox('Sound Stats', 'debugSound')}
      ${checkbox('Hydraulic', 'debugHydraulic')}
    </div>
  </div>`;
}

function panelSounds() {
  const s = CFG.sounds || {};
  return `<div class="panel" id="p-sounds">

    <div class="section-title">Engine Start</div>
    <div class="field"><label>Sound File</label>${soundSelect('startSound', s.startSound)}</div>
    ${numField('Start Volume %', 'startVolumePercentage', 0, 500, 5)}

    <div class="section-title">Idle</div>
    <div class="field"><label>Sound File</label>${soundSelect('idleSound', s.idleSound)}</div>
    ${numField('Idle Volume %', 'idleVolumePercentage', 0, 500, 5)}
    ${numField('Engine Idle Volume %', 'engineIdleVolumePercentage', 0, 300, 5)}
    ${numField('Full Throttle Volume %', 'fullThrottleVolumePercentage', 0, 500, 5)}

    <div class="section-title">Rev Sound</div>
    <div class="check-group">${checkbox('Enable Rev Sound', 'revSoundEnabled')}</div>
    <div class="field"><label>Sound File</label>${soundSelect('revSound', s.revSound)}</div>
    ${numField('Rev Volume %', 'revVolumePercentage', 0, 500, 5)}
    ${numField('Engine Rev Volume %', 'engineRevVolumePercentage', 0, 300, 5)}
    ${numInput('Rev Switch Point', 'revSwitchPoint', 0, 500)}
    ${numInput('Idle End Point', 'idleEndPoint', 0, 500)}
    ${numField('Idle Volume Proportion %', 'idleVolumeProportionPercentage', 0, 100, 5)}

    <div class="section-title">Diesel Knock</div>
    <div class="field"><label>Sound File</label>${soundSelect('knockSound', s.knockSound)}</div>
    ${numField('Knock Volume %', 'dieselKnockVolumePercentage', 0, 1000, 10)}
    ${numField('Knock Idle Volume %', 'dieselKnockIdleVolumePercentage', 0, 100, 1)}
    ${numInput('Knock Interval (cylinders)', 'dieselKnockInterval', 1, 12)}
    ${numInput('Knock Start Point', 'dieselKnockStartPoint', 0, 500)}
    ${numField('Knock Adaptive Volume %', 'dieselKnockAdaptiveVolumePercentage', 0, 100, 5)}

    <div class="section-title">Turbo</div>
    <div class="field"><label>Sound File</label>${soundSelect('turboSound', s.turboSound)}</div>
    ${numField('Turbo Volume %', 'turboVolumePercentage', 0, 200, 5)}
    ${numField('Turbo Idle Volume %', 'turboIdleVolumePercentage', 0, 100, 5)}

    <div class="section-title">Supercharger</div>
    <div class="field"><label>Sound File</label>${soundSelect('chargerSound', s.chargerSound)}</div>
    ${numField('Charger Volume % (0=off)', 'chargerVolumePercentage', 0, 200, 5)}
    ${numField('Charger Idle Volume %', 'chargerIdleVolumePercentage', 0, 100, 5)}
    ${numInput('Charger Start Point', 'chargerStartPoint', 0, 500)}

    <div class="section-title">Wastegate</div>
    <div class="field"><label>Sound File</label>${soundSelect('wastegateSound', s.wastegateSound)}</div>
    ${numField('Wastegate Volume %', 'wastegateVolumePercentage', 0, 200, 5)}
    ${numField('Wastegate Idle Volume %', 'wastegateIdleVolumePercentage', 0, 100, 5)}

    <div class="section-title">Fan</div>
    <div class="field"><label>Sound File</label>${soundSelect('fanSound', s.fanSound)}</div>
    ${numField('Fan Volume % (0=off)', 'fanVolumePercentage', 0, 200, 5)}
    ${numField('Fan Idle Volume %', 'fanIdleVolumePercentage', 0, 100, 5)}
    ${numInput('Fan Start Point', 'fanStartPoint', 0, 500)}

    <div class="section-title">Engine Tuning</div>
    ${numField('Max RPM % (200=diesel, 400=gas)', 'MAX_RPM_PERCENTAGE', 100, 500, 10)}
    ${numField('Acceleration (1=slow, 9=fast)', 'acc', 1, 9, 1)}
    ${numField('Deceleration (1=slow, 9=fast)', 'dec', 1, 9, 1)}
    ${numInput('Clutch Engaging Point', 'clutchEngagingPoint', 0, 500)}

    <div class="section-title" style="margin-top:24px;">Hydraulic Pump</div>
    <div class="field"><label>Sound File</label>${soundSelect('hydraulicPumpSound', s.hydraulicPumpSound)}</div>
    ${numField('Pump Volume %', 'hydraulicPumpVolumePercentage', 0, 500, 5)}

    <div class="section-title">Hydraulic Flow</div>
    <div class="field"><label>Sound File</label>${soundSelect('hydraulicFlowSound', s.hydraulicFlowSound)}</div>
    ${numField('Flow Volume %', 'hydraulicFlowVolumePercentage', 0, 500, 5)}

    <div class="section-title">Track Rattle</div>
    <div class="field"><label>Sound File</label>${soundSelect('trackRattleSound', s.trackRattleSound)}</div>
    ${numField('Track Rattle Volume %', 'trackRattleVolumePercentage', 0, 500, 5)}
    <div class="check-group">${checkbox('Enable Track Rattle 2 (periodic clank)', 'trackRattle2Enabled')}</div>
    ${numField('Rattle 2 Volume %', 'trackRattle2VolumePercentage', 0, 500, 5)}
    ${numInput('Rattle Interval Min (ms at max speed)', 'trackRattleIntervalMin', 10, 2000)}
    ${numInput('Rattle Interval Max (ms at min speed)', 'trackRattleIntervalMax', 50, 5000)}
    ${numInput('Chain Drive Top Speed PWM', 'pwmStrokeChainDriveTopSpeed', 1, 255)}
    ${numInput('Chain Drive Start Rotation', 'pwmStrokeChainDriveStartRotation', 0, 255)}

    <div class="section-title">Bucket Rattle</div>
    <div class="field"><label>Sound File</label>${soundSelect('bucketRattleSound', s.bucketRattleSound)}</div>
    ${numField('Bucket Rattle Volume %', 'bucketRattleVolumePercentage', 0, 500, 5)}

    <div class="section-title">Hydraulic Response</div>
    ${numInput('Ramp Time (ms)', 'hydraulicRampTime', 50, 1000)}
    ${numInput('Dead Zone (us)', 'hydraulicDeadZone', 0, 200)}

    <div class="section-title" style="margin-top:24px;">Horn</div>
    <div class="field"><label>Sound File</label>${soundSelect('hornSound', s.hornSound)}</div>
    ${numField('Horn Volume %', 'hornVolumePercentage', 0, 500, 5)}

    <div class="section-title">Siren</div>
    <div class="field"><label>Sound File</label>${soundSelect('sirenSound', s.sirenSound)}</div>
    ${numField('Siren Volume %', 'sirenVolumePercentage', 0, 500, 5)}

    <div class="section-title">Air Brake</div>
    <div class="field"><label>Sound File</label>${soundSelect('brakeSound', s.brakeSound)}</div>
    ${numField('Brake Volume %', 'brakeVolumePercentage', 0, 500, 5)}

    <div class="section-title">Shifting</div>
    <div class="field"><label>Sound File</label>${soundSelect('shiftingSound', s.shiftingSound)}</div>
    ${numField('Shifting Volume %', 'shiftingVolumePercentage', 0, 300, 5)}

    <div class="section-title">Misc Sound (door etc.)</div>
    <div class="field"><label>Sound File</label>${soundSelect('sound1Sound', s.sound1Sound)}</div>
    ${numField('Sound1 Volume %', 'sound1VolumePercentage', 0, 300, 5)}

    <div class="section-title">Reversing Beep</div>
    <div class="field"><label>Sound File</label>${soundSelect('reversingSound', s.reversingSound)}</div>
    ${numField('Reversing Volume %', 'reversingVolumePercentage', 0, 300, 5)}

    <div class="section-title">Indicator</div>
    <div class="field"><label>Sound File</label>${soundSelect('indicatorSound', s.indicatorSound)}</div>
    ${numField('Indicator Volume %', 'indicatorVolumePercentage', 0, 300, 5)}
    ${numInput('Indicator On (ms)', 'indicatorOn', 50, 2000)}
    <div class="check-group">${checkbox('Indicator Direction', 'INDICATOR_DIR')}</div>

    <div class="section-title">Coupling</div>
    <div class="field"><label>Sound File</label>${soundSelect('couplingSound', s.couplingSound)}</div>
    ${numField('Coupling Volume %', 'couplingVolumePercentage', 0, 300, 5)}

    <div class="section-title">Uncoupling</div>
    <div class="field"><label>Sound File</label>${soundSelect('uncouplingSound', s.uncouplingSound)}</div>
  </div>`;
}

function panelRC() {
  return `<div class="panel" id="p-rc">
    <div class="section-title">RC Protocol</div>
    ${radioGroup('rcProtocol', [
      {value:'SBUS_COMMUNICATION', label:'SBUS'},
      {value:'IBUS_COMMUNICATION', label:'IBUS'},
      {value:'SUMD_COMMUNICATION', label:'SUMD'},
      {value:'PPM_COMMUNICATION', label:'PPM'},
      {value:'PWM_COMMUNICATION', label:'PWM'},
    ])}
    <div class="section-title">SBUS Settings</div>
    <div class="field">
      <label>Signal Inverted</label>
      <select onchange="CFG.sbusInverted=this.value">
        <option value="true" ${CFG.sbusInverted==='true'?'selected':''}>true (standard)</option>
        <option value="false" ${CFG.sbusInverted==='false'?'selected':''}>false</option>
      </select>
    </div>
    ${numInput('SBUS Baud Rate', 'sbusBaud', 90000, 200000)}
    <div class="section-title">Signal Range</div>
    ${numInput('Neutral Dead Zone (us)', 'pulseNeutral', 10, 100)}
    ${numInput('Pulse Span (us)', 'pulseSpan', 200, 500)}
  </div>`;
}

function panelESC() {
  return `<div class="panel" id="p-esc">
    <div class="section-title">Hi/Lo Range (2-Speed / Rabbit Mode)</div>
    <div class="check-group">
      ${checkbox('Enable Hi/Lo Range', 'hiLoEnabled')}
      ${checkbox('Default to High Range', 'hiLoDefaultHigh')}
    </div>
    ${numField('Low Range Speed % (of full)', 'hiLoRatioPercent', 10, 100, 5)}
    <p class="hint">CH6 toggle switches between High and Low range. Low range limits top speed to the percentage above. Great for fine jobsite maneuvering.</p>
    <div class="section-title">ESC Ramp Times (ms per step)</div>
    ${numField('1st Gear Ramp', 'escRampTimeFirstGear', 1, 100, 1)}
    ${numField('2nd Gear Ramp', 'escRampTimeSecondGear', 1, 150, 1)}
    ${numField('3rd Gear Ramp', 'escRampTimeThirdGear', 1, 200, 1)}
    <div class="section-title">ESC Response</div>
    ${numField('Brake Steps', 'escBrakeSteps', 1, 100, 1)}
    ${numField('Acceleration Steps', 'escAccelerationSteps', 1, 20, 1)}
    <div class="section-title">Transmission</div>
    <div class="check-group">
      ${checkbox('Automatic', 'automatic')}
      ${checkbox('Double Clutch', 'doubleClutch')}
      ${checkbox('Shifting Auto Throttle', 'shiftingAutoThrottle')}
    </div>
  </div>`;
}

function panelServos() {
  const min = CFG.servoMin || [1000,1000,1000,1000,1000];
  const max = CFG.servoMax || [2000,2000,2000,2000,2000];
  const ctr = CFG.servoCenter || [1500,1500,1500,1500,1500];
  const labels = getChannelLabels();

  // Build all available functions for this machine mode (machine-specific first, then common)
  const allFuncs = [...(CH_MAP_MACHINE[CFG.machineType] || []), ...CH_MAP_COMMON];

  function chSelect(varName, label) {
    const val = CFG[varName] || 0;
    const opts = Array.from({length:17}, (_,n) =>
      '<option value="' + n + '"' + (n===val?' selected':'') + '>' + (n===0 ? 'OFF' : 'CH'+n) + '</option>'
    ).join('');
    return '<div style="display:flex;align-items:center;gap:8px;margin-bottom:4px;">' +
      '<span style="min-width:130px;font-size:12px;color:var(--dim);">' + label + '</span>' +
      '<select style="background:var(--surface);border:1px solid var(--border);color:var(--text);' +
        'padding:4px 8px;border-radius:6px;font-size:12px;font-family:var(--font);width:80px;"' +
        ' onchange="CFG[\'' + varName + '\']=+this.value;onMachineChange()">' +
        opts +
      '</select>' +
    '</div>';
  }

  return `<div class="panel" id="p-servos">
    <div class="section-title">Channel Mapping &mdash; <span style="color:var(--dim);font-weight:400;text-transform:none;font-size:11px">${MACHINE_NAMES[CFG.machineType] || ''} mode</span></div>
    <p class="hint" style="margin-bottom:10px;">Assign each function to a receiver channel. <strong>OFF</strong> = disabled. Use <strong>Push to ESP32</strong> to apply â€” no rebuild needed!</p>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:4px 32px;margin-bottom:16px;">
      ${allFuncs.map(m => chSelect(m.v, m.label)).join('')}
    </div>

    <div class="section-title">Channel Reverse</div>
    <p class="hint" style="margin-bottom:8px;">Flip the direction of individual channels. Useful when a servo or stick moves the wrong way.</p>
    <div style="display:grid;grid-template-columns:repeat(4, 1fr);gap:4px 16px;margin-bottom:16px;">
      ${Array.from({length:16},(_,i)=>i+1).map(ch => {
        const rev = CFG.channelReversed && CFG.channelReversed[ch];
        return '<div style="display:flex;align-items:center;gap:6px;padding:3px 0;">' +
          '<label class="sw"><input type="checkbox" ' + (rev ? 'checked' : '') +
          ' onchange="if(!CFG.channelReversed)CFG.channelReversed={};CFG.channelReversed[' + ch + ']=this.checked"><span class="sl"></span></label>' +
          '<span style="font-size:11px;color:var(--dim);">CH' + ch + ' ' + (labels[ch-1]||'') + '</span></div>';
      }).join('')}
    </div>

    <div class="section-title">&#9889; Push to ESP32 &mdash; <span style="color:var(--dim);font-weight:400;text-transform:none;font-size:11px">No rebuild needed!</span></div>
    <p class="hint" style="margin-bottom:8px;">Send channel mapping + live settings directly to the connected ESP32 over serial. Saved to flash â€” survives reboots.</p>
    <div style="display:flex;gap:8px;align-items:center;margin-bottom:8px;flex-wrap:wrap;">
      <select id="serialPort" style="background:var(--surface);border:1px solid var(--border);color:var(--text);padding:6px 10px;border-radius:6px;font-size:12px;font-family:var(--font);min-width:160px;">
        <option value="">Select COM port...</option>
      </select>
      <button class="btn btn-ghost btn-sm" onclick="refreshPorts()">&#8635; Refresh</button>
      <button class="btn btn-primary btn-sm" onclick="pushChannels()" style="font-weight:700;">&#9889; Push All to ESP32</button>
      <span id="pushStatus" style="font-size:11px;color:var(--dim);"></span>
    </div>
    <pre id="pushLog" style="display:none;background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:8px 12px;font-size:11px;color:var(--text);max-height:200px;overflow-y:auto;margin-top:6px;white-space:pre-wrap;"></pre>

    <div class="section-title">&#127911; Live Tuning &mdash; <span style="color:var(--dim);font-weight:400;text-transform:none;font-size:11px">Runtime-configurable (pushed via serial)</span></div>
    <p class="hint" style="margin-bottom:10px;">Adjust volumes, engine response, and ESC tuning. Push to ESP32 to apply instantly.</p>

    <div style="display:grid;grid-template-columns:1fr 1fr;gap:12px 24px;margin-bottom:16px;">
      <div>
        <div style="font-size:11px;font-weight:700;color:var(--accent);text-transform:uppercase;margin-bottom:6px;">Engine</div>
        ${liveSlider('masterVolume', 'Master Volume', 0, 200, '%')}
        ${liveSlider('idleVolumePercentage', 'Idle Volume', 0, 300, '%')}
        ${liveSlider('dieselKnockVolumePercentage', 'Diesel Knock', 0, 1000, '%')}
        ${liveSlider('turboVolumePercentage', 'Turbo Whistle', 0, 300, '%')}
        ${liveSlider('startVolumePercentage', 'Start Sound', 0, 300, '%')}
        ${liveSlider('acc', 'Acceleration (inertia)', 1, 9, '')}
        ${liveSlider('dec', 'Deceleration (inertia)', 1, 9, '')}
        ${liveToggle('autoEngineStart', 'Auto Engine Start', 'Engine starts on throttle (no switch needed)')}
      </div>
      <div>
        <div style="font-size:11px;font-weight:700;color:var(--accent);text-transform:uppercase;margin-bottom:6px;">Sounds &amp; ESC</div>
        ${liveSlider('hornVolumePercentage', 'Horn', 0, 300, '%')}
        ${liveSlider('brakeVolumePercentage', 'Air Brake', 0, 300, '%')}
        ${liveSlider('reversingVolumePercentage', 'Reversing Beep', 0, 300, '%')}
        ${liveSlider('hydraulicPumpVolumePercentage', 'Hyd. Pump', 0, 300, '%')}
        ${liveSlider('hydraulicFlowVolumePercentage', 'Hyd. Flow', 0, 300, '%')}
        ${liveSlider('trackRattleVolumePercentage', 'Track Rattle', 0, 300, '%')}
        ${liveSlider('bucketRattleVolumePercentage', 'Bucket Rattle', 0, 300, '%')}
        ${liveSlider('escRampTimeLow', 'ESC Ramp (Low)', 5, 200, 'ms')}
        ${liveSlider('escRampTimeHigh', 'ESC Ramp (High)', 5, 200, 'ms')}
        ${liveSlider('escBrakeSteps', 'Brake Steps', 1, 100, '')}
        ${liveSlider('escAccelerationSteps', 'Accel Steps', 1, 20, '')}
      </div>
    </div>

    <div class="section-title">Servo Endpoints (&micro;s)</div>
    <div class="servo-grid">
      <div class="hdr"></div>${[0,1,2,3,4].map(i=>`<div class="hdr ch-hdr">${labels[i] || 'CH'+(i+1)}</div>`).join('')}
      <div class="lbl">Min</div>${min.map((v,i)=>`<input type="number" value="${v}" min="500" max="2500" onchange="CFG.servoMin[${i}]=+this.value">`).join('')}
      <div class="lbl">Center</div>${ctr.map((v,i)=>`<input type="number" value="${v}" min="500" max="2500" onchange="CFG.servoCenter[${i}]=+this.value">`).join('')}
      <div class="lbl">Max</div>${max.map((v,i)=>`<input type="number" value="${v}" min="500" max="2500" onchange="CFG.servoMax[${i}]=+this.value">`).join('')}
    </div>
  </div>`;
}

function panelBuild() {
  return `<div class="panel" id="p-build">
    <div style="display:flex; gap:8px; margin-bottom:12px; align-items:center">
      <button class="btn btn-primary btn-sm" onclick="startBuild(false)">&#9881; Build</button>
      <button class="btn btn-primary btn-sm" onclick="startBuild(true)">&#9889; Build &amp; Flash</button>
      <span id="buildStatus"></span>
    </div>
    <div id="buildOutput">Click Build to compile firmware...</div>
  </div>`;
}

// â”€â”€ Live Sound Builder â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
let _stAllSounds = [];
let _stFilterCat = 'all';
let _sbAudioCtx = null;
let _sbSource = null;
let _sbGain = null;
let _sbBuffer = null;
let _sbPlaying = false;
let _sbCurrentFile = '';
let _sbLoopBuf = null;
let _sbLoopStartTime = 0;
let _sbSwapTimer = null;
let _sbHighPass = null;
let _sbLowPass = null;
let _sbRawSamples = null;
let _sbRawRate = 22050;

function _getAudioCtx() {
  if (!_sbAudioCtx) _sbAudioCtx = new (window.AudioContext || window.webkitAudioContext)();
  return _sbAudioCtx;
}

function sbUpdateFilters() {
  const ctx = _getAudioCtx();
  if (_sbHighPass) {
    const hp = parseInt(document.getElementById('sbHighPass').value) || 0;
    const hpVal = hp > 0 ? hp : 10;
    _sbHighPass.frequency.setValueAtTime(_sbHighPass.frequency.value, ctx.currentTime);
    _sbHighPass.frequency.linearRampToValueAtTime(hpVal, ctx.currentTime + 0.1);
  }
  if (_sbLowPass) {
    const lp = parseInt(document.getElementById('sbLowPass').value) || 11025;
    _sbLowPass.frequency.setValueAtTime(_sbLowPass.frequency.value, ctx.currentTime);
    _sbLowPass.frequency.linearRampToValueAtTime(lp, ctx.currentTime + 0.1);
  }
}

function _sbConnectChain(source, ctx) {
  const hp = parseInt(document.getElementById('sbHighPass').value) || 0;
  const lp = parseInt(document.getElementById('sbLowPass').value) || 11025;
  _sbHighPass = ctx.createBiquadFilter();
  _sbHighPass.type = 'highpass';
  _sbHighPass.frequency.value = hp > 0 ? hp : 10;
  _sbHighPass.Q.value = 0.7;
  _sbLowPass = ctx.createBiquadFilter();
  _sbLowPass.type = 'lowpass';
  _sbLowPass.frequency.value = lp;
  _sbLowPass.Q.value = 0.7;
  const volSlider = document.getElementById('sbVolSlider');
  _sbGain = ctx.createGain();
  _sbGain.gain.value = (volSlider ? parseInt(volSlider.value) : 100) / 100;
  source.connect(_sbHighPass);
  _sbHighPass.connect(_sbLowPass);
  _sbLowPass.connect(_sbGain);
  _sbGain.connect(ctx.destination);
  return _sbGain;
}

function resampleLinear(floatData, fromRate, toRate) {
  if (fromRate === toRate) return floatData;
  const ratio = fromRate / toRate;
  const outLen = Math.max(1, Math.floor(floatData.length / ratio));
  const out = new Float32Array(outLen);
  for (let i = 0; i < outLen; i++) {
    const pos = i * ratio;
    const idx = Math.floor(pos);
    const frac = pos - idx;
    const a = floatData[idx] || 0;
    const b = floatData[Math.min(floatData.length - 1, idx + 1)] || a;
    out[i] = a + (b - a) * frac;
  }
  return out;
}

function crossfadeLoop(pcm8arr, fadeSamples) {
  if (!fadeSamples || fadeSamples < 2 || pcm8arr.length < fadeSamples * 2) return pcm8arr;
  const out = new Int8Array(pcm8arr.length - fadeSamples);
  for (let i = fadeSamples; i < pcm8arr.length - fadeSamples; i++) out[i] = pcm8arr[i];
  for (let i = 0; i < fadeSamples; i++) {
    const t = i / fadeSamples;
    const fromEnd = pcm8arr[pcm8arr.length - fadeSamples + i];
    const fromStart = pcm8arr[i];
    out[i] = Math.max(-128, Math.min(127, Math.round(fromEnd * (1 - t) + fromStart * t)));
  }
  return out;
}

function compressPcm8(pcm8arr, amount) {
  if (!amount || amount <= 0 || pcm8arr.length < 100) return pcm8arr;
  const len = pcm8arr.length;
  const winSize = Math.max(32, Math.min(512, Math.floor(len / 50)));
  const half = Math.floor(winSize / 2);
  const env = new Float32Array(len);
  let sumSq = 0;
  for (let i = 0; i < Math.min(winSize, len); i++) sumSq += (pcm8arr[i] / 128.0) ** 2;
  for (let i = 0; i < len; i++) {
    const addIdx = i + half;
    const remIdx = i - half - 1;
    if (addIdx < len) sumSq += (pcm8arr[addIdx] / 128.0) ** 2;
    if (remIdx >= 0) sumSq -= (pcm8arr[remIdx] / 128.0) ** 2;
    if (sumSq < 0) sumSq = 0;
    const cnt = Math.min(addIdx + 1, len) - Math.max(remIdx + 1, 0);
    env[i] = Math.sqrt(sumSq / cnt);
  }
  let totalSq = 0;
  for (let i = 0; i < len; i++) totalSq += (pcm8arr[i] / 128.0) ** 2;
  const targetRms = Math.sqrt(totalSq / len);
  if (targetRms < 0.001) return pcm8arr;
  const out = new Int8Array(len);
  for (let i = 0; i < len; i++) {
    const localRms = Math.max(env[i], 0.005);
    const gain = targetRms / localRms;
    const clampedGain = Math.min(gain, 4.0);
    const finalGain = 1.0 + (clampedGain - 1.0) * amount;
    const sample = pcm8arr[i] * finalGain;
    out[i] = Math.max(-128, Math.min(127, Math.round(sample)));
  }
  return out;
}

function _sbResample(samples, factor) {
  if (Math.abs(factor - 1.0) < 0.001) return samples;
  const newLen = Math.max(4, Math.round(samples.length * factor));
  const out = new Int8Array(newLen);
  for (let i = 0; i < newLen; i++) {
    const srcPos = i / factor;
    const idx = Math.floor(srcPos);
    const frac = srcPos - idx;
    const s0 = idx < samples.length ? samples[idx] : 0;
    const s1 = (idx + 1) < samples.length ? samples[idx + 1] : s0;
    out[i] = Math.max(-128, Math.min(127, Math.round(s0 * (1 - frac) + s1 * frac)));
  }
  return out;
}

function _sbBuildPreviewBuf(ctx) {
  if (!_sbRawSamples) return null;
  const dur = _sbBuffer.duration;
  const startSlider = document.getElementById('sbLoopStart');
  const endSlider = document.getElementById('sbLoopEnd');
  const ls = parseFloat(startSlider ? startSlider.value : '0');
  const le = parseFloat(endSlider ? endSlider.value : '1');
  const startIdx = Math.max(0, Math.floor(ls * _sbRawSamples.length));
  const endIdx = Math.min(_sbRawSamples.length, Math.floor(le * _sbRawSamples.length));
  if (endIdx - startIdx < 4) return null;
  let slice = _sbRawSamples.slice(startIdx, endIdx);
  const cfPct = parseInt(document.getElementById('sbCrossfade').value) || 0;
  const minFade = 32;
  if (slice.length > 200) {
    const fadeSamples = Math.max(minFade, Math.floor(slice.length * cfPct / 100));
    if (slice.length > fadeSamples * 2) slice = crossfadeLoop(slice, fadeSamples);
  }
  const pitchSt = parseFloat(document.getElementById('sbPitch') ? document.getElementById('sbPitch').value : '0');
  const pitchLock = !!(document.getElementById('sbPitchLock') && document.getElementById('sbPitchLock').checked);
  let pitchFactor = Math.pow(2, -pitchSt / 12);
  if (pitchLock) {
    const rpm = parseFloat(document.getElementById('sbRpmSlider') ? document.getElementById('sbRpmSlider').value : '1');
    pitchFactor *= (1 / rpm);
  }
  if (Math.abs(pitchFactor - 1.0) > 0.001) slice = _sbResample(slice, pitchFactor);
  const buf = ctx.createBuffer(1, slice.length, _sbRawRate);
  const ch = buf.getChannelData(0);
  for (let i = 0; i < slice.length; i++) ch[i] = slice[i] / 128.0;
  return buf;
}

function sbStop() {
  if (_sbSource) { try { _sbSource.stop(); } catch(e) {} _sbSource = null; }
  if (_sbHighPass) { try { _sbHighPass.disconnect(); } catch(e) {} _sbHighPass = null; }
  if (_sbLowPass) { try { _sbLowPass.disconnect(); } catch(e) {} _sbLowPass = null; }
  if (_sbGain) { try { _sbGain.disconnect(); } catch(e) {} _sbGain = null; }
  _sbPlaying = false;
  updatePlayBtn();
}

function sbPlayStop() { if (_sbPlaying) sbStop(); else sbPlay(); }

function updatePlayBtn() {
  const btn = document.getElementById('sbPlayBtn');
  if (btn) btn.innerHTML = _sbPlaying ? '&#9724; Stop' : '&#9654; Play';
}

function sbPlay() {
  if (!_sbBuffer) return;
  sbStop();
  const ctx = _getAudioCtx();
  const dur = _sbBuffer.duration;
  const startSlider = document.getElementById('sbLoopStart');
  const endSlider = document.getElementById('sbLoopEnd');
  const ls = parseFloat(startSlider ? startSlider.value : '0') * dur;
  const le = parseFloat(endSlider ? endSlider.value : '1') * dur;
  const doLoop = !!(document.getElementById('sbLoop') && document.getElementById('sbLoop').checked);
  if (doLoop) {
    const previewBuf = _sbBuildPreviewBuf(ctx);
    if (previewBuf) {
      _sbLoopBuf = previewBuf;
      _sbSource = ctx.createBufferSource();
      _sbSource.buffer = previewBuf;
      _sbSource.loop = true;
      _sbSource.loopStart = 0;
      _sbSource.loopEnd = previewBuf.duration;
      const rpmSlider = document.getElementById('sbRpmSlider');
      _sbSource.playbackRate.value = rpmSlider ? parseFloat(rpmSlider.value) : 1.0;
      _sbConnectChain(_sbSource, ctx);
      _sbSource.onended = function() { _sbPlaying = false; updatePlayBtn(); };
      _sbSource.start(0);
      _sbLoopStartTime = ctx.currentTime;
      _sbPlaying = true;
      updatePlayBtn();
      return;
    }
  }
  _sbSource = ctx.createBufferSource();
  _sbSource.buffer = _sbBuffer;
  _sbSource.loop = doLoop;
  _sbSource.loopStart = ls;
  _sbSource.loopEnd = le;
  const rpmSlider = document.getElementById('sbRpmSlider');
  _sbSource.playbackRate.value = rpmSlider ? parseFloat(rpmSlider.value) : 1.0;
  _sbConnectChain(_sbSource, ctx);
  _sbSource.onended = function() { _sbPlaying = false; updatePlayBtn(); };
  _sbSource.start(0, ls);
  _sbLoopStartTime = ctx.currentTime;
  _sbPlaying = true;
  updatePlayBtn();
}

function sbUpdateRpm(val) {
  const v = parseFloat(val);
  const label = document.getElementById('sbRpmLabel');
  let desc = 'idle';
  if (v >= 2.5) desc = 'redline';
  else if (v >= 2.0) desc = 'high RPM';
  else if (v >= 1.5) desc = 'mid-high';
  else if (v >= 1.1) desc = 'mid RPM';
  else if (v >= 0.7) desc = 'idle';
  else desc = 'very low';
  if (label) label.textContent = v.toFixed(2) + 'x (' + desc + ')';
  if (_sbSource && _sbPlaying) {
    const ctx = _getAudioCtx();
    _sbSource.playbackRate.setValueAtTime(_sbSource.playbackRate.value, ctx.currentTime);
    _sbSource.playbackRate.linearRampToValueAtTime(v, ctx.currentTime + 0.15);
    const pitchLock = !!(document.getElementById('sbPitchLock') && document.getElementById('sbPitchLock').checked);
    if (pitchLock) {
      if (_sbSwapTimer) clearTimeout(_sbSwapTimer);
      _sbSwapTimer = setTimeout(_sbHotSwap, 200);
    }
  }
}

function sbUpdateVol(val) {
  const v = parseInt(val);
  const label = document.getElementById('sbVolLabel');
  if (label) label.textContent = v + '%';
  if (_sbGain) {
    const ctx = _getAudioCtx();
    _sbGain.gain.setValueAtTime(_sbGain.gain.value, ctx.currentTime);
    _sbGain.gain.linearRampToValueAtTime(v / 100, ctx.currentTime + 0.08);
  }
}

function sbUpdateLoopPoints() {
  if (!_sbBuffer) return;
  const dur = _sbBuffer.duration;
  const startSlider = document.getElementById('sbLoopStart');
  const endSlider = document.getElementById('sbLoopEnd');
  const startLabel = document.getElementById('sbLoopStartLabel');
  const endLabel = document.getElementById('sbLoopEndLabel');
  const selInfo = document.getElementById('sbSelectionInfo');
  let ls = parseFloat(startSlider.value) * dur;
  let le = parseFloat(endSlider.value) * dur;
  if (le <= ls) le = Math.min(ls + 0.001, dur);
  if (startLabel) startLabel.textContent = ls.toFixed(3) + 's';
  if (endLabel) endLabel.textContent = le.toFixed(3) + 's';
  const selSamples = Math.round((le - ls) * _sbRawRate);
  const selKB = Math.round(selSamples / 1024);
  if (selInfo) selInfo.textContent = 'Selection: ' + (le - ls).toFixed(3) + 's, ~' + selSamples + ' samples, ~' + selKB + ' KB';
  if (_sbPlaying && _sbSource) {
    if (_sbSwapTimer) clearTimeout(_sbSwapTimer);
    _sbSwapTimer = setTimeout(_sbHotSwap, 120);
  }
}

function _sbHotSwap() {
  if (!_sbPlaying || !_sbSource || !_sbBuffer) return;
  const ctx = _getAudioCtx();
  const newBuf = _sbBuildPreviewBuf(ctx);
  if (!newBuf) return;
  const rate = _sbSource.playbackRate.value;
  const elapsed = (ctx.currentTime - _sbLoopStartTime) * rate;
  const oldDur = _sbLoopBuf ? _sbLoopBuf.duration : _sbBuffer.duration;
  const phase = elapsed % oldDur;
  const offset = Math.min(phase / oldDur * newBuf.duration, newBuf.duration - 0.001);
  const fadeTime = 0.03;
  const volSlider = document.getElementById('sbVolSlider');
  const vol = (volSlider ? parseInt(volSlider.value) : 100) / 100;
  const oldSource = _sbSource;
  const oldGain = _sbGain;
  oldSource.onended = null;
  oldGain.gain.setValueAtTime(oldGain.gain.value, ctx.currentTime);
  oldGain.gain.linearRampToValueAtTime(0, ctx.currentTime + fadeTime);
  try { oldSource.stop(ctx.currentTime + fadeTime + 0.01); } catch(e) {}
  setTimeout(function(){ try { oldGain.disconnect(); } catch(e){} }, 100);
  _sbLoopBuf = newBuf;
  _sbSource = ctx.createBufferSource();
  _sbSource.buffer = newBuf;
  _sbSource.loop = true;
  _sbSource.loopStart = 0;
  _sbSource.loopEnd = newBuf.duration;
  _sbSource.playbackRate.value = rate;
  _sbConnectChain(_sbSource, ctx);
  _sbGain.gain.setValueAtTime(0, ctx.currentTime);
  _sbGain.gain.linearRampToValueAtTime(vol, ctx.currentTime + fadeTime);
  _sbSource.onended = function() { _sbPlaying = false; updatePlayBtn(); };
  _sbSource.start(0, offset > 0 ? offset : 0);
  _sbLoopStartTime = ctx.currentTime - (offset / (rate || 1));
}

function sbUpdatePitch() {
  const pitchSlider = document.getElementById('sbPitch');
  const pitchLabel = document.getElementById('sbPitchLabel');
  const v = parseFloat(pitchSlider ? pitchSlider.value : '0');
  if (pitchLabel) pitchLabel.textContent = (v >= 0 ? '+' : '') + v.toFixed(1) + ' st';
  if (_sbPlaying) {
    if (_sbSwapTimer) clearTimeout(_sbSwapTimer);
    _sbSwapTimer = setTimeout(_sbHotSwap, 120);
  }
}

async function stPreviewCurrent(slot) {
  const sel = document.querySelector('[data-sound="' + slot + '"]');
  if (!sel) return;
  const file = sel.value;
  if (!file) return;
  sbStop();
  try {
    const r = await fetch('/api/sound_pcm/' + encodeURIComponent(file));
    const j = await r.json();
    if (!j.ok) return;
    const ctx = _getAudioCtx();
    const buf = ctx.createBuffer(1, j.samples.length, j.sampleRate);
    const ch = buf.getChannelData(0);
    for (let i = 0; i < j.samples.length; i++) ch[i] = j.samples[i] / 128.0;
    _sbBuffer = buf;
    _sbRawSamples = j.samples;
    _sbRawRate = j.sampleRate;
    _sbCurrentFile = file;
    _sbSource = ctx.createBufferSource();
    _sbSource.buffer = buf;
    _sbSource.loop = true;
    _sbGain = ctx.createGain();
    _sbGain.gain.value = 1.0;
    _sbSource.connect(_sbGain);
    _sbGain.connect(ctx.destination);
    _sbSource.onended = function() { _sbPlaying = false; };
    _sbSource.start(0);
    _sbPlaying = true;
  } catch(e) { console.error('stPreviewCurrent error:', e); }
}

async function stLoadBrowser() {
  try {
    const r = await fetch('/api/all_sounds');
    _stAllSounds = await r.json();
  } catch(e) { _stAllSounds = []; }
  stRenderBrowser();
}

function stRenderBrowser() {
  const search = (document.getElementById('stSearch')?.value || '').toLowerCase();
  const filtered = _stAllSounds.filter(s => {
    if (_stFilterCat !== 'all' && s.category !== _stFilterCat) return false;
    if (search && !s.label.toLowerCase().includes(search)) return false;
    return true;
  });
  const tbody = document.getElementById('stBrowserBody');
  if (!tbody) return;
  const countEl = document.getElementById('stCount');
  if (countEl) countEl.textContent = filtered.length + ' / ' + _stAllSounds.length;
  const catColors = {idle:'#4ade80',rev:'#f87171',start:'#fbbf24',knock:'#f97316',
    horn:'#60a5fa',siren:'#f472b6',brake:'#94a3b8',turbo:'#22d3ee',wastegate:'#a78bfa',
    track:'#a3e635',hydraulic:'#22d3ee',other:'#666'};
  tbody.innerHTML = filtered.map(s => {
    const color = catColors[s.category] || '#888';
    const hl = s.file === _sbCurrentFile ? 'background:rgba(255,203,5,0.08);' : '';
    return '<tr style="border-bottom:1px solid var(--border);cursor:pointer;' + hl + '" onclick="sbLoadSound(\'' + s.file.replace(/'/g, "\\'") + '\')">' +
      '<td style="padding:5px 8px;font-size:12px;color:var(--text);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:220px" title="' + s.label + '">' +
      (s.file === _sbCurrentFile ? '&#9654; ' : '') + s.label + '</td>' +
      '<td style="padding:5px 4px;text-align:center"><span style="color:' + color + ';font-size:10px;background:var(--surface);padding:1px 6px;border-radius:8px">' + s.category + '</span></td>' +
      '<td style="padding:5px 4px;text-align:center"><button class="btn btn-ghost btn-sm" onclick="event.stopPropagation();sbLoadAndPlay(\'' + s.file.replace(/'/g, "\\'") + '\')" title="Load & Play">&#9654;</button></td></tr>';
  }).join('');
}

async function sbLoadSound(filename) {
  sbStop();
  _sbCurrentFile = filename;
  const nameEl = document.getElementById('sbNowPlaying');
  if (nameEl) nameEl.textContent = 'Loading ' + filename + '...';
  const info = document.getElementById('sbSoundInfo');
  if (info) info.textContent = '';
  try {
    const r = await fetch('/api/sound_pcm/' + encodeURIComponent(filename));
    const j = await r.json();
    if (!j.ok) { if (nameEl) nameEl.textContent = 'Error: ' + (j.error || '?'); return; }
    const ctx = _getAudioCtx();
    const buf = ctx.createBuffer(1, j.samples.length, j.sampleRate);
    const ch = buf.getChannelData(0);
    for (let i = 0; i < j.samples.length; i++) ch[i] = j.samples[i] / 128.0;
    _sbBuffer = buf;
    _sbRawSamples = j.samples;
    _sbRawRate = j.sampleRate;
    const dur = (j.samples.length / j.sampleRate).toFixed(3);
    if (nameEl) nameEl.textContent = filename.replace('.h', '');
    if (info) info.textContent = j.sampleRate + ' Hz, ' + j.samples.length + ' samples, ' + dur + 's';
    const startSlider = document.getElementById('sbLoopStart');
    const endSlider = document.getElementById('sbLoopEnd');
    if (startSlider) startSlider.value = 0;
    if (endSlider) endSlider.value = 1;
    sbUpdateLoopPoints();
    stRenderBrowser();
  } catch(e) { if (nameEl) nameEl.textContent = 'Error loading: ' + e; }
}

async function sbLoadAndPlay(filename) {
  await sbLoadSound(filename);
  sbPlay();
}

// Import a WAV file from the user's computer into the Live Sound Editor
async function sbImportWav(fileInput) {
  if (!fileInput.files || !fileInput.files[0]) return;
  const file = fileInput.files[0];
  const nameEl = document.getElementById('sbNowPlaying');
  const info = document.getElementById('sbSoundInfo');
  if (nameEl) nameEl.textContent = 'Importing ' + file.name + '...';

  sbStop();

  try {
    const arrayBuf = await file.arrayBuffer();
    const ctx = _getAudioCtx();
    const decoded = await ctx.decodeAudioData(arrayBuf);

    // Mix down to mono if stereo
    let monoData;
    if (decoded.numberOfChannels === 1) {
      monoData = decoded.getChannelData(0);
    } else {
      const ch0 = decoded.getChannelData(0);
      const ch1 = decoded.getChannelData(1);
      monoData = new Float32Array(ch0.length);
      for (let i = 0; i < ch0.length; i++) monoData[i] = (ch0[i] + ch1[i]) * 0.5;
    }

    // Resample to a reasonable rate if the source is very high (>44100)
    const srcRate = decoded.sampleRate;
    let targetRate = srcRate;
    let samples = monoData;
    if (srcRate > 44100) {
      targetRate = 22050;
      const ratio = srcRate / targetRate;
      const newLen = Math.floor(monoData.length / ratio);
      const resampled = new Float32Array(newLen);
      for (let i = 0; i < newLen; i++) {
        const srcIdx = i * ratio;
        const lo = Math.floor(srcIdx);
        const hi = Math.min(lo + 1, monoData.length - 1);
        const frac = srcIdx - lo;
        resampled[i] = monoData[lo] * (1 - frac) + monoData[hi] * frac;
      }
      samples = resampled;
    }

    // Convert float [-1, 1] to int8 [-128, 127] for internal representation
    const int8Samples = new Array(samples.length);
    for (let i = 0; i < samples.length; i++) {
      int8Samples[i] = Math.max(-128, Math.min(127, Math.round(samples[i] * 128)));
    }

    // Create AudioBuffer at the target rate
    const buf = ctx.createBuffer(1, samples.length, targetRate);
    const ch = buf.getChannelData(0);
    for (let i = 0; i < samples.length; i++) ch[i] = int8Samples[i] / 128.0;

    _sbBuffer = buf;
    _sbRawSamples = int8Samples;
    _sbRawRate = targetRate;
    _sbCurrentFile = file.name.replace(/\.wav$/i, '.h');

    const dur = (samples.length / targetRate).toFixed(3);
    if (nameEl) nameEl.textContent = file.name.replace(/\.wav$/i, '') + ' (imported)';
    if (info) info.textContent = targetRate + ' Hz, ' + samples.length + ' samples, ' + dur + 's (from ' + srcRate + ' Hz WAV)';

    // Reset loop sliders
    const startSlider = document.getElementById('sbLoopStart');
    const endSlider = document.getElementById('sbLoopEnd');
    if (startSlider) startSlider.value = 0;
    if (endSlider) endSlider.value = 1;
    sbUpdateLoopPoints();

    toast('Imported ' + file.name + ' \u2014 use Export .h or Install when ready', true);
  } catch(e) {
    if (nameEl) nameEl.textContent = 'Import error: ' + e.message;
    toast('Failed to import WAV: ' + e.message, false);
  }
  // Reset file input so same file can be re-imported
  fileInput.value = '';
}

function sbGetLoopRegion() {
  if (!_sbBuffer) return null;
  const dur = _sbBuffer.duration;
  const startSlider = document.getElementById('sbLoopStart');
  const endSlider = document.getElementById('sbLoopEnd');
  const ls = parseFloat(startSlider ? startSlider.value : '0') * dur;
  const le = parseFloat(endSlider ? endSlider.value : '1') * dur;
  return { start: ls, end: le };
}

function sbProcessSlice() {
  const region = sbGetLoopRegion();
  const startIdx = Math.max(0, Math.floor(region.start * _sbRawRate));
  const endIdx = Math.min(_sbRawSamples.length, Math.floor(region.end * _sbRawRate));
  if (endIdx <= startIdx) return null;
  let slice = _sbRawSamples.slice(startIdx, endIdx);
  const speed = parseFloat(document.getElementById('sbRpmSlider').value) || 1;
  let outRate = parseInt(document.getElementById('sbExportRate').value) || 22050;
  if (speed !== 1) {
    const floats = new Float32Array(slice.length);
    for (let i = 0; i < slice.length; i++) floats[i] = slice[i] / 128.0;
    const resampled = resampleLinear(floats, _sbRawRate * speed, _sbRawRate);
    slice = new Int8Array(resampled.length);
    for (let i = 0; i < resampled.length; i++) slice[i] = Math.max(-128, Math.min(127, Math.round(resampled[i] * 128)));
  }
  if (outRate !== _sbRawRate) {
    const floats = new Float32Array(slice.length);
    for (let i = 0; i < slice.length; i++) floats[i] = slice[i] / 128.0;
    const resampled = resampleLinear(floats, _sbRawRate, outRate);
    slice = new Int8Array(resampled.length);
    for (let i = 0; i < resampled.length; i++) slice[i] = Math.max(-128, Math.min(127, Math.round(resampled[i] * 128)));
  }
  const smoothPct = parseInt(document.getElementById('sbSmooth').value) || 0;
  if (smoothPct > 0) slice = compressPcm8(slice, smoothPct / 100);
  if (document.getElementById('sbExportNorm').checked) {
    let peak = 0;
    for (let i = 0; i < slice.length; i++) peak = Math.max(peak, Math.abs(slice[i]));
    if (peak > 0 && peak < 127) {
      const gain = 127 / peak;
      const normed = new Int8Array(slice.length);
      for (let i = 0; i < slice.length; i++) normed[i] = Math.max(-128, Math.min(127, Math.round(slice[i] * gain)));
      slice = normed;
    }
  }
  const cfPct = parseInt(document.getElementById('sbCrossfade').value) || 0;
  if (cfPct > 0 && slice.length > 200) {
    const fadeSamples = Math.max(2, Math.floor(slice.length * cfPct / 100));
    slice = crossfadeLoop(slice, fadeSamples);
  }
  return { slice: slice, rate: outRate, region: region, speed: speed };
}

function sbBuildHeader(varName, slice, rate, region, speed) {
  const lines = [];
  lines.push('// Exported from Live Sound Builder');
  lines.push('// Loop region: ' + region.start.toFixed(3) + 's - ' + region.end.toFixed(3) + 's');
  if (speed !== 1) lines.push('// Export speed: ' + speed + 'x');
  lines.push('#pragma once');
  lines.push('const unsigned int ' + varName + '_sampleRate = ' + rate + ';');
  lines.push('const unsigned int ' + varName + '_sampleCount = ' + slice.length + ';');
  lines.push('const signed char ' + varName + '[] = {');
  let row = '  ';
  for (let i = 0; i < slice.length; i++) {
    row += slice[i].toString();
    if (i !== slice.length - 1) row += ', ';
    if ((i + 1) % 20 === 0 && i !== slice.length - 1) { lines.push(row); row = '  '; }
  }
  if (row.trim()) lines.push(row);
  lines.push('};');
  return lines.join('\\n');
}

function sbExportSelection() {
  if (!_sbRawSamples || !_sbBuffer) { toast('Load a sound first', false); return; }
  const p = sbProcessSlice();
  if (!p) { toast('Invalid selection', false); return; }
  const varName = _sbCurrentFile.replace('.h', '').replace(/[^a-zA-Z0-9_]/g, '');
  const text = sbBuildHeader(varName, p.slice, p.rate, p.region, p.speed);
  const blob = new Blob([text], {type: 'text/plain'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = varName + '.h';
  a.click();
  URL.revokeObjectURL(a.href);
  const sizeKB = Math.round(p.slice.length / 1024);
  const dur = (p.slice.length / p.rate).toFixed(2);
  toast('Exported ' + varName + '.h \\u2014 ' + p.slice.length + ' samples (' + dur + 's @ ' + p.rate + 'Hz), ~' + sizeKB + ' KB', true);
}

async function sbInstallSelection() {
  if (!_sbRawSamples || !_sbBuffer) { toast('Load a sound first', false); return; }
  const p = sbProcessSlice();
  if (!p) { toast('Invalid selection', false); return; }
  const varName = _sbCurrentFile.replace('.h', '').replace(/[^a-zA-Z0-9_]/g, '');
  const filename = varName + '.h';
  const text = sbBuildHeader(varName, p.slice, p.rate, p.region, p.speed);
  try {
    const resp = await fetch('/api/install_sound', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ filename: filename, content: text })
    });
    const data = await resp.json();
    if (data.ok) {
      toast('Installed ' + filename + ' to sounds folder', true);
      stLoadBrowser();
    } else {
      toast('Install failed: ' + (data.error || '?'), false);
    }
  } catch(e) { toast('Install error: ' + e, false); }
}

function panelSoundTech() {
  const s = CFG.sounds || {};

  const cats = ['all', ...new Set(_stAllSounds.map(s=>s.category))].sort();
  const catOpts = cats.map(c => `<option value="${c}">${c==='all'?'All Categories':c}</option>`).join('');

  return `<div class="panel" id="p-soundtech">
      '<option value="">(use compiled default)</option>' + opts + '</select></td></tr>';
  }).join('');

  return `<div class="panel" id="p-soundtech">
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:16px;">
      <!-- Sound Browser (left) -->
      <div>
        <div class="section-title">Sound Browser</div>
        <p class="hint" style="margin-bottom:8px;">Browse and preview all available sounds. Click <strong>Load</strong> to open in the Live Sound Editor.</p>
        <div style="display:flex;gap:6px;margin-bottom:10px;align-items:center;">
          <select id="stCatFilter" style="background:var(--surface);border:1px solid var(--border);color:var(--text);padding:5px 8px;border-radius:6px;font-size:11px;font-family:var(--font);"
            onchange="_stFilterCat=this.value;stRenderBrowser()">
            ${catOpts}
          </select>
          <input type="text" id="stSearch" placeholder="Search..."
            style="background:var(--surface);border:1px solid var(--border);color:var(--text);padding:5px 8px;border-radius:6px;font-size:11px;font-family:var(--font);flex:1;"
            oninput="stRenderBrowser()">
          <button class="btn btn-ghost btn-sm" onclick="sbStop()">&#9632; Stop</button>
        </div>
        <div style="max-height:560px;overflow-y:auto;border:1px solid var(--border);border-radius:var(--radius);">
          <table style="width:100%;border-collapse:collapse;">
            <thead><tr style="background:var(--surface);position:sticky;top:0;">
              <th style="padding:6px 8px;text-align:left;font-size:10px;color:var(--accent);text-transform:uppercase;">Name</th>
              <th style="padding:6px 8px;text-align:left;font-size:10px;color:var(--accent);text-transform:uppercase;">Category</th>
              <th style="padding:6px 8px;text-align:center;font-size:10px;color:var(--accent);text-transform:uppercase;">Actions</th>
            </tr></thead>
            <tbody id="stBrowserBody"></tbody>
          </table>
        </div>
      </div>

      <!-- Live Sound Editor (right) -->
      <div>
        <div class="section-title">&#127925; Live Sound Editor</div>
        <div style="padding:8px;background:var(--surface);border-radius:var(--radius);border:1px solid var(--border)">
          <div style="display:flex;align-items:center;gap:12px;flex-wrap:wrap">
            <span style="color:var(--accent);font-size:13px;font-weight:bold" id="sbNowPlaying">No sound loaded</span>
            <button type="button" class="btn btn-primary btn-sm" onclick="sbPlayStop()" id="sbPlayBtn" style="min-width:60px">&#9654; Play</button>
            <label style="font-size:12px;color:var(--dim)"><input id="sbLoop" type="checkbox" checked> Loop</label>
            <button type="button" class="btn btn-ghost btn-sm" onclick="document.getElementById('wavFileInput').click()" title="Import a .wav file from your computer">&#128194; Import WAV</button>
            <input type="file" id="wavFileInput" accept=".wav,audio/wav" style="display:none" onchange="sbImportWav(this)">
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:8px;flex-wrap:wrap">
            <label style="color:var(--dim);font-size:12px;white-space:nowrap">RPM:</label>
            <input id="sbRpmSlider" type="range" min="0.3" max="3.0" step="0.05" value="1.0"
              style="flex:1;min-width:150px;accent-color:var(--accent)"
              oninput="sbUpdateRpm(this.value)">
            <span id="sbRpmLabel" style="color:var(--text);font-size:13px;min-width:70px">1.00x (idle)</span>
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:6px;flex-wrap:wrap">
            <label style="color:var(--dim);font-size:12px;white-space:nowrap">Volume:</label>
            <input id="sbVolSlider" type="range" min="0" max="200" step="5" value="100"
              style="width:100px;accent-color:var(--accent)"
              oninput="sbUpdateVol(this.value)">
            <span id="sbVolLabel" style="color:var(--text);font-size:12px">100%</span>
            <span style="color:var(--dim);font-size:11px;margin-left:auto" id="sbSoundInfo"></span>
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:8px;flex-wrap:wrap">
            <label style="color:#4ade80;font-size:12px;white-space:nowrap">Loop Start:</label>
            <input id="sbLoopStart" type="range" min="0" max="1" step="0.001" value="0"
              style="flex:1;min-width:120px;accent-color:#4ade80"
              oninput="sbUpdateLoopPoints()">
            <span id="sbLoopStartLabel" style="color:#4ade80;font-size:11px;min-width:50px">0.000s</span>
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:4px;flex-wrap:wrap">
            <label style="color:#f87171;font-size:12px;white-space:nowrap">Loop End:</label>
            <input id="sbLoopEnd" type="range" min="0" max="1" step="0.001" value="1"
              style="flex:1;min-width:120px;accent-color:#f87171"
              oninput="sbUpdateLoopPoints()">
            <span id="sbLoopEndLabel" style="color:#f87171;font-size:11px;min-width:50px">1.000s</span>
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:6px;flex-wrap:wrap">
            <label style="color:var(--text);font-size:12px;white-space:nowrap">Smooth:</label>
            <input id="sbSmooth" type="range" min="0" max="100" step="5" value="0"
              style="flex:1;min-width:100px;accent-color:var(--accent)"
              oninput="document.getElementById('sbSmoothLabel').textContent=this.value+'%'; if(_sbPlaying){if(_sbSwapTimer)clearTimeout(_sbSwapTimer);_sbSwapTimer=setTimeout(_sbHotSwap,150);}">
            <span id="sbSmoothLabel" style="color:var(--text);font-size:11px;min-width:30px">0%</span>
            <span style="color:var(--dim);font-size:10px">(evens out loud &amp; quiet spots)</span>
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:6px;flex-wrap:wrap">
            <label style="color:#f472b6;font-size:12px;white-space:nowrap">Crossfade:</label>
            <input id="sbCrossfade" type="range" min="0" max="100" step="1" value="10"
              style="flex:1;min-width:100px;accent-color:#f472b6"
              oninput="document.getElementById('sbCrossfadeLabel').textContent=this.value+'%'; if(_sbPlaying){if(_sbSwapTimer)clearTimeout(_sbSwapTimer);_sbSwapTimer=setTimeout(_sbHotSwap,150);}">
            <span id="sbCrossfadeLabel" style="color:#f472b6;font-size:11px;min-width:30px">10%</span>
            <span style="color:var(--dim);font-size:10px">(blends end&rarr;start for seamless loop)</span>
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:6px;flex-wrap:wrap">
            <label style="color:#60a5fa;font-size:12px;white-space:nowrap">Low Cut:</label>
            <input id="sbHighPass" type="range" min="0" max="2000" step="10" value="0"
              style="flex:1;min-width:100px;accent-color:#60a5fa"
              oninput="document.getElementById('sbHighPassLabel').textContent=this.value+'Hz'; sbUpdateFilters();">
            <span id="sbHighPassLabel" style="color:#60a5fa;font-size:11px;min-width:40px">0Hz</span>
            <span style="color:var(--dim);font-size:10px">(removes rumble)</span>
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:6px;flex-wrap:wrap">
            <label style="color:#a78bfa;font-size:12px;white-space:nowrap">High Cut:</label>
            <input id="sbLowPass" type="range" min="500" max="11025" step="25" value="11025"
              style="flex:1;min-width:100px;accent-color:#a78bfa"
              oninput="document.getElementById('sbLowPassLabel').textContent=this.value+'Hz'; sbUpdateFilters();">
            <span id="sbLowPassLabel" style="color:#a78bfa;font-size:11px;min-width:50px">11025Hz</span>
            <span style="color:var(--dim);font-size:10px">(removes hiss)</span>
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:6px;flex-wrap:wrap">
            <label style="color:#e879f9;font-size:12px;white-space:nowrap">Pitch:</label>
            <input id="sbPitch" type="range" min="-12" max="12" step="0.5" value="0"
              style="flex:1;min-width:100px;accent-color:#e879f9"
              oninput="sbUpdatePitch()">
            <span id="sbPitchLabel" style="color:#e879f9;font-size:11px;min-width:50px">0 st</span>
            <label style="font-size:11px;color:var(--dim);white-space:nowrap" title="When locked, pitch stays constant regardless of RPM speed"><input id="sbPitchLock" type="checkbox" onchange="sbUpdatePitch()"> Lock</label>
            <span style="color:var(--dim);font-size:10px">(semitones)</span>
          </div>
          <p style="color:var(--dim);font-size:11px;margin:8px 0 0;padding-top:8px;border-top:1px solid var(--border)">Export bakes in your current RPM, loop points, and pitch settings.</p>
          <div style="display:flex;align-items:center;gap:8px;margin-top:6px;flex-wrap:wrap">
            <label style="color:var(--accent);font-size:12px;white-space:nowrap">Rate:</label>
            <select id="sbExportRate" style="width:80px;background:var(--surface);border:1px solid var(--border);color:var(--text);padding:4px 6px;border-radius:6px;font-size:12px;font-family:var(--font);" title="Output sample rate">
              <option value="8000">8 kHz</option>
              <option value="11025">11 kHz</option>
              <option value="16000">16 kHz</option>
              <option value="22050" selected>22 kHz</option>
            </select>
            <label style="font-size:12px;color:var(--dim)"><input id="sbExportNorm" type="checkbox" checked> Normalize</label>
          </div>
          <div style="display:flex;align-items:center;gap:8px;margin-top:8px;flex-wrap:wrap">
            <button type="button" class="btn btn-primary btn-sm" onclick="sbExportSelection()" title="Export the selected loop region as a .h header file">&#128229; Export .h</button>
            <button type="button" class="btn btn-primary btn-sm" onclick="sbInstallSelection()" title="Export and install to sounds folder">&#9654; Install</button>
            <span id="sbSelectionInfo" style="color:var(--dim);font-size:11px"></span>
          </div>
        </div>
      </div>
    </div>

    <style>
      .st-playing { outline:2px solid var(--accent) !important; animation:pulse 1s infinite; }
      #p-soundtech table tr:hover td { background:rgba(255,203,5,0.05); }
      #p-soundtech input[type=range] { accent-color: var(--accent); }
    </style>
  </div>`;
}

// â”€â”€ Serial Push (no-rebuild channel config) â”€â”€â”€â”€â”€
async function refreshPorts() {
  const sel = document.getElementById('serialPort');
  if (!sel) return;
  try {
    const r = await fetch('/api/serial_ports');
    const ports = await r.json();
    const prev = sel.value;
    sel.innerHTML = '<option value="">Select COM port...</option>';
    ports.forEach(p => {
      const opt = document.createElement('option');
      opt.value = p.port;
      opt.textContent = p.port + (p.desc && p.desc !== p.port ? ' â€” ' + p.desc : '');
      sel.appendChild(opt);
    });
    if (prev) sel.value = prev;
  } catch(e) { console.error('refreshPorts:', e); }
}

async function pushChannels() {
  const port = document.getElementById('serialPort').value;
  if (!port) { toast('Select a COM port first', false); return; }
  const statusEl = document.getElementById('pushStatus');
  const logEl = document.getElementById('pushLog');
  if (statusEl) statusEl.textContent = 'Pushing...';
  if (logEl) { logEl.style.display = 'block'; logEl.textContent = 'Connecting to ' + port + '...\n'; }

  // Gather all channel mappings (0 = unassigned/none)
  const all = [...CH_MAP_COMMON, ...(CH_MAP_MACHINE[CFG.machineType] || [])];
  const channels = {};
  for (const m of all) {
    channels[m.v] = CFG[m.v] !== undefined ? CFG[m.v] : 0;
  }

  // Gather runtime settings
  const SETTINGS_KEYS = [
    'masterVolume', 'idleVolumePercentage', 'dieselKnockVolumePercentage',
    'turboVolumePercentage', 'hornVolumePercentage', 'brakeVolumePercentage',
    'hydraulicPumpVolumePercentage', 'hydraulicFlowVolumePercentage',
    'trackRattleVolumePercentage', 'bucketRattleVolumePercentage',
    'reversingVolumePercentage', 'startVolumePercentage',
    'acc', 'dec', 'escRampTimeLow', 'escRampTimeHigh',
    'escBrakeSteps', 'escAccelerationSteps',
  ];
  const settings = {};
  for (const k of SETTINGS_KEYS) {
    if (CFG[k] !== undefined) settings[k] = CFG[k];
  }
  // Boolean setting
  const autoStart = (CFG.autoEngineStart === 'true' || CFG.autoEngineStart === true);
  settings['autoEngineStart'] = autoStart ? 1 : 0;

  // Gather channel reverse flags
  const reversed = CFG.channelReversed || {};
  // Gather channel enable flags
  const enabled = CFG.channelEnabled || {};

  try {
    const resp = await fetch('/api/push_channels', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ port: port, channels: channels, settings: settings, channelReversed: reversed, channelEnabled: enabled })
    });
    const data = await resp.json();
    if (logEl && data.log) logEl.textContent += data.log.join('\n') + '\n';
    if (data.ok) {
      if (statusEl) statusEl.innerHTML = '<span style="color:#2ecc71;">&#10003; Pushed &amp; saved!</span>';
      toast('Channels + settings pushed to ESP32!', true);
    } else {
      if (statusEl) statusEl.innerHTML = '<span style="color:var(--danger);">Failed: ' + data.error + '</span>';
      toast('Push failed: ' + data.error, false);
    }
  } catch(e) {
    if (statusEl) statusEl.innerHTML = '<span style="color:var(--danger);">Error: ' + e + '</span>';
    toast('Push error: ' + e, false);
  }
}

// â”€â”€ Save â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
async function saveConfig() {
  CFG.sounds = CFG.sounds || {};
  document.querySelectorAll('[data-sound]').forEach(sel => {
    CFG.sounds[sel.dataset.sound] = sel.value;
  });
  const resp = await fetch('/api/save', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify(CFG)
  });
  const data = await resp.json();
  toast(data.ok ? 'Config saved!' : ('Error: ' + data.error), data.ok);
}

// â”€â”€ Build â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
let buildPoll = null;
async function startBuild(upload) {
  await saveConfig();
  const url = upload ? '/api/upload' : '/api/build';
  await fetch(url, {method:'POST'});
  toast(upload ? 'Build & flash started...' : 'Build started...', true);
  pollBuild();
}

function pollBuild() {
  if (buildPoll) clearInterval(buildPoll);
  buildPoll = setInterval(async () => {
    const resp = await fetch('/api/build-log');
    const data = await resp.json();
    const el = document.getElementById('buildOutput');
    if (el) { el.textContent = data.log.join('\n') || 'No output yet...'; el.scrollTop = el.scrollHeight; }
    const st = document.getElementById('buildStatus');
    if (st) {
      if (data.running) {
        st.innerHTML = '<span class="status-dot busy"></span> Building...';
      } else if (data.log.length > 0) {
        const last = data.log[data.log.length-1] || '';
        const ok = last.includes('Exit code: 0') || data.log.some(l => l.includes('[SUCCESS]'));
        st.innerHTML = ok
          ? '<span class="status-dot ok"></span> Success'
          : '<span class="status-dot" style="background:var(--danger)"></span> Failed';
        if (!data.running) clearInterval(buildPoll);
      }
    }
  }, 1000);
}

// â”€â”€ Toast â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
function toast(msg, ok) {
  const div = document.createElement('div');
  div.className = 'toast ' + (ok ? 'ok' : 'err');
  div.textContent = msg;
  document.body.appendChild(div);
  setTimeout(() => { div.style.opacity = '0'; setTimeout(() => div.remove(), 300); }, 2500);
}

init();
