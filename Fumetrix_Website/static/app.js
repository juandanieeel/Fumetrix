const originalFetch=window.fetch.bind(window); window.fetch=async (...args)=>{const r=await originalFetch(...args);if(r.status===401){location.assign('/login');throw new Error('Sign in required');}return r;};
const canControl=['admin','operator'].includes(document.querySelector('meta[name=user-role]').content);
'use strict';
const $ = id => document.getElementById(id);
const dateText = value => value ? new Date(value).toLocaleString('en-PH', {timeZone:'Asia/Manila'}) : '—';
const shown = value => value === null || value === undefined ? '—' : String(value);
let currentView = 'overview';
function renderSystemStatusVisibility(data) {
  const maintenance = data?.connected && data.state === 'AWAKE' && data.mode === 'MAINTENANCE';
  $('systemStatusPanel').hidden = currentView !== 'overview' || !maintenance;
  $('outputStatusPanel').hidden = currentView !== 'overview' || !!maintenance;
}
function showView(name) {
  if (!['overview','history'].includes(name)) name='overview';
  currentView=name;
  document.querySelectorAll('.view').forEach(v=>{v.hidden=v.id!==name;});
  document.querySelectorAll('[data-view]').forEach(button=>{
    if (button.dataset.view===name) button.setAttribute('aria-current','page');
    else button.removeAttribute('aria-current');
  });
  if(name==='history') {historyPage=1;loadHistory();}
  if(name==='overview') {drawChart();loadOverview();}
  renderSystemStatusVisibility(lastDevice);
  $('sensorCheckPanel').hidden=name!=='overview'||lastDevice?.mode!=='MAINTENANCE';
  $('thresholdPanel').hidden=name!=='overview'||lastDevice?.mode!=='MAINTENANCE';
}
document.querySelectorAll('[data-view]').forEach(b=>b.addEventListener('click',()=>{location.hash=b.dataset.view;}));
document.querySelectorAll('[data-open]').forEach(b=>b.addEventListener('click',()=>{location.hash=b.dataset.open;}));
window.addEventListener('hashchange',()=>showView(location.hash.slice(1)));
$('openHistory').addEventListener('click',()=>{location.hash='history';});
$('showSleepHistory').addEventListener('click',()=>{$('kind').value='sleep_sessions';historyPage=1;if(currentView==='history') loadHistory();else location.hash='history';});

// Modal supports keyboard navigation, Escape, and returning focus to its opener.
let opener;
function closeModal(){ $('modalBackdrop').classList.remove('open'); opener?.focus(); }
$('btnSettings').addEventListener('click',()=>{opener=document.activeElement;$('modalBackdrop').classList.add('open');$('modalClose').focus();});
$('modalClose').addEventListener('click',closeModal);
$('modalBackdrop').addEventListener('click',e=>{if(e.target===$('modalBackdrop')) closeModal();});
document.addEventListener('keydown',e=>{
  if(!$('modalBackdrop').classList.contains('open')) return;
  if(e.key==='Escape') closeModal();
  if(e.key==='Tab'){e.preventDefault();$('modalClose').focus();} // Only interactive element in this informational dialog.
});

function query(){return new URLSearchParams({kind:$('kind').value,start:$('start').value,end:$('end').value});}
async function getJSON(url){
  const response=await fetch(url,{signal:AbortSignal.timeout(8000)});
  const data=await response.json();
  if(!response.ok) throw new Error(data.error||'Request failed.');
  return data;
}
let historyVersion=0, historyPage=1;
function elapsedText(seconds) {
  if (!Number.isInteger(seconds)) return '—';
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor(seconds / 60) % 60;
  return [hours, minutes].map(value => String(value).padStart(2, '0')).join(':');
}
async function loadHistory(){
  const version=++historyVersion, params=query();
  $('export').href='/api/export?'+params;
  $('exportPdf').href='/api/export/pdf?'+params;
  params.set('page',historyPage);
  $('historyPrev').disabled=$('historyNext').disabled=true;
  $('historyStatus').textContent='Loading database records…';
  $('tableHead').replaceChildren();$('tableBody').replaceChildren();
  try{
    const data=await getJSON('/api/history?'+params);
    if(version!==historyVersion) return;
    const fields=data.kind==='readings'
      ? [['recorded_at','Saved to website'],['elapsed_s','SD elapsed'],['mode','Mode'],['temperature','Temp °C'],['humidity','RH %'],['pm1','PM1.0 µg/m³'],['pm25','PM2.5 µg/m³'],['pm10','PM10 µg/m³'],['voc_index','VOC index'],['nox_index','NOx index']]
      : [['sleep_at','Sleep started'],['wake_at','Woke up'],['duration_seconds','Duration (s)'],['quality','Record quality']];
    const heading=document.createElement('tr');
    for(const [,label] of fields){const th=document.createElement('th');th.scope='col';th.textContent=label;heading.append(th);}
    $('tableHead').append(heading);
    for(const row of data.rows){const tr=document.createElement('tr');for(const [key] of fields){const td=document.createElement('td');td.textContent=key==='elapsed_s'?elapsedText(row[key]):key.endsWith('_at')?dateText(row[key]):shown(row[key]);tr.append(td);}$('tableBody').append(tr);}
    historyPage=data.page;
    $('historyPage').textContent='Page '+data.page+' of '+data.pages;
    $('historyPrev').disabled=data.page<=1;
    $('historyNext').disabled=data.page>=data.pages;
    $('historyStatus').textContent=data.total?data.total+' matching records.':'No matching records.';
  }catch(error){
    if(version===historyVersion) {
      $('historyPage').textContent='';
      $('historyStatus').textContent=error.message||'Cannot reach database. Try again.';
    }
  }
}
$('filters').addEventListener('submit',e=>{e.preventDefault();historyPage=1;loadHistory();});
$('kind').addEventListener('change',()=>{historyPage=1;loadHistory();});
$('historyPrev').addEventListener('click',()=>{historyPage--;loadHistory();});
$('historyNext').addEventListener('click',()=>{historyPage++;loadHistory();});
const clearHistoryButton=$('clearHistory');
clearHistoryButton?.addEventListener('click',async()=>{
  const kind=$('kind').value;
  const label=kind==='readings'?'sensor readings':'sleep sessions';
  if(!window.confirm('Permanently clear all '+label+' from the website database, including records outside the date filter?')) return;
  clearHistoryButton.disabled=true;
  $('clearHistoryStatus').textContent='Clearing history…';
  try {
    const response=await fetch('/api/history/clear',{
      method:'POST',
      headers:{'Content-Type':'application/json','X-CSRF-Token':document.querySelector('meta[name=csrf-token]').content},
      body:JSON.stringify({kind}),
      signal:AbortSignal.timeout(8000)
    });
    const data=await response.json();
    if(!response.ok) throw new Error(data.error||'Could not clear history.');
    $('clearHistoryStatus').textContent='Cleared '+data.deleted+' '+label+'. New records will continue to appear.';
    historyPage=1;
    await loadHistory();
    loadOverview();
  } catch(error) {
    $('clearHistoryStatus').textContent=error.message||'Could not clear history.';
  } finally {
    clearHistoryButton.disabled=false;
  }
});

let chartRows=[];
function drawChart(){
  if(currentView!=='overview') return;
  const canvas=$('mainChart'), w=canvas.clientWidth, h=190, dpr=window.devicePixelRatio||1;
  if(!w) return;
  canvas.width=Math.round(w*dpr);canvas.height=h*dpr;
  const ctx=canvas.getContext('2d');ctx.scale(dpr,dpr);ctx.clearRect(0,0,w,h);
  const theme = getComputedStyle(document.documentElement);
  const chartLabel = theme.getPropertyValue('--chart-label').trim();
  const chartGrid = theme.getPropertyValue('--chart-grid').trim();
  const chartDot = theme.getPropertyValue('--chart-dot').trim();
  const left=44,right=w-12,top=15,bottom=h-30;
  const field=$('chartSensor').value;
  const valid=chartRows.filter(r=>Number.isFinite(r[field])&&Number.isFinite(Date.parse(r.recorded_at)));
  const min=Math.min(0,...valid.map(r=>r[field]));
  const max=Math.max(1,...valid.map(r=>r[field]))*1.15;
  ctx.font='10px monospace';ctx.fillStyle=chartLabel;
  for(let i=0;i<=3;i++){
    const y=bottom-(bottom-top)*i/3;
    ctx.strokeStyle=chartGrid;ctx.beginPath();ctx.moveTo(left,y);ctx.lineTo(right,y);ctx.stroke();
    if(valid.length)ctx.fillText((min+(max-min)*i/3).toFixed(0),3,y+3);
  }
  if(!valid.length){ctx.textAlign='center';ctx.fillText('No saved measurements for this sensor',w/2,h/2);ctx.textAlign='start';return;}
  const times=valid.map(r=>Date.parse(r.recorded_at)), first=Math.min(...times),last=Math.max(...times);
  const x=t=>first===last?(left+right)/2:left+(t-first)/(last-first)*(right-left);
  const y=value=>bottom-(value-min)/(max-min)*(bottom-top);
  // Dots preserve gaps and avoid implying continuous coverage between logged samples.
  ctx.fillStyle=chartDot;
  valid.forEach((r,i)=>{ctx.beginPath();ctx.arc(x(times[i]),y(r[field]),3,0,Math.PI*2);ctx.fill();});
  ctx.fillStyle=chartLabel;ctx.font='9px monospace';
  const label=t=>new Date(t).toLocaleString('en-PH',{timeZone:'Asia/Manila',month:'short',day:'numeric',hour:'2-digit',minute:'2-digit'});
  ctx.fillText(label(first),left,h-8);ctx.textAlign='right';ctx.fillText(label(last),right,h-8);ctx.textAlign='start';
}
window.addEventListener('resize',drawChart);
window.addEventListener('fumetrix:themechange',drawChart);
$('chartSensor').addEventListener('change',drawChart);
async function loadOverview(){
  try{
    const data=await getJSON('/api/history?kind=readings&recent=1');
    chartRows=data.rows.slice(0,90).reverse();drawChart();
    $('chartStatus').textContent=chartRows.length?'Latest '+chartRows.length+' saved readings · historical data, not live telemetry':'No stored measurements yet. Real logs will appear after device integration.';
    $('recentBody').replaceChildren();
    data.rows.slice(0,5).forEach(row=>{const tr=document.createElement('tr');for(const value of [dateText(row.recorded_at),shown(row.temperature)]){const td=document.createElement('td');td.textContent=value;tr.append(td);}$('recentBody').append(tr);});
    $('recentEmpty').hidden=data.rows.length>0;
    $('recentEmpty').textContent='No stored measurements yet.';
  }catch{
    chartRows=[];drawChart();$('recentBody').replaceChildren();$('recentEmpty').hidden=false;
    $('recentEmpty').textContent='Cannot load stored measurements.';
    $('chartStatus').textContent='Database history unavailable. Reconnecting…';
  }
}
let sending=false, lastDevice=null, localTarget=null;
let rejectedCommand = null;
const modeTargets = {
  mode_auto: 'MODE:AUTO',
  mode_maintenance: 'MODE:MAINTENANCE',
  sleep: 'SLEEPING',
  shutdown: 'SLEEPING',
  wake: 'AWAKE'
};
const requestedModes = {
  'MODE:AUTO': 'AUTO',
  'MODE:MAINTENANCE': 'MAINTENANCE',
  SLEEPING: 'SLEEP',
  AWAKE: 'AUTO'
};
let statusTimer, statusBusy=false, statusAgain=false, statusEpoch=0;
function scheduleStatus(delay=0){clearTimeout(statusTimer);statusTimer=setTimeout(poll,delay);}
async function poll(){
  if(statusBusy){statusAgain=true;return;}
  statusBusy=true;
  const epoch=statusEpoch;
  try{
    const data=await getJSON('/api/status');
    if(epoch===statusEpoch){
      $('serverState').textContent='Online';$('backendBadge').textContent='Online';$('backendBadge').className='conn-badge online';
      $('databaseState').textContent='Cloudflare D1: connected';$('recordCount').textContent=data.reading_count;
      renderDevice(data);
    }
  }catch{
    if(epoch===statusEpoch){
      $('serverState').textContent='Offline';$('backendBadge').textContent='Offline';$('backendBadge').className='conn-badge offline';
      $('databaseState').textContent='Cloudflare D1: unavailable';$('recordCount').textContent='—';
      renderDevice({connected:false,state:'UNKNOWN',message:'Website server unavailable.'});
    }
  }finally{
    statusBusy=false;
    const fast=sending||lastDevice?.command_pending||['WAKING','SLEEP_PENDING'].includes(lastDevice?.state);
    scheduleStatus(statusAgain?0:fast?250:document.hidden?15000:1000);
    statusAgain=false;
  }
}
// Charts and history never delay device status or command acknowledgement.
async function refreshHistory(){
  if(!document.hidden) {
    if(currentView==='history') await loadHistory();
    else await loadOverview();
  }
  setTimeout(refreshHistory,60000);
}
document.addEventListener('visibilitychange',()=>{
  if(!document.hidden) scheduleStatus();
});

function reading(id,value,unit){
  const box=$(id);box.replaceChildren();
  box.append(document.createTextNode(Number.isFinite(value)?value.toFixed(2):'—'));
  const suffix=document.createElement('span');suffix.className='unit';suffix.textContent=unit;box.append(suffix);
}
function renderRecommendations(data, state) {
  const panel = $('recommendationPanel');
  const status = $('recommendationStatus'), list = $('recommendationList');
  panel.hidden = state === 'SLEEPING' || data.mode === 'MAINTENANCE';
  if (panel.hidden) return;
  if (state === 'AWAKE' && data.saved_thresholds === undefined) return;

  const limits = data.saved_thresholds;
  const readings = [
    ['PM2.5', data.pm_status === 'READY' ? data.pm25 : null, 'µg/m³', null, 'pm25_max', '',
      'Check for soldering fumes or dust and confirm the extractor is running.'],
    ['VOC', data.gas_status === 'READY' ? data.voc_index : null, 'index', null, 'voc_max', '',
      'Check for nearby fumes or solvents and improve ventilation.'],
    ['NOx', data.gas_status === 'READY' ? data.nox_index : null, 'index', null, 'nox_max', '',
      'Check for a nearby combustion source and improve ventilation.'],
    ['Temperature', data.sht_status === 'READY' ? data.temperature : null, '°C', 'temp_min', 'temp_max',
      'Check for cold drafts near the sensor.', 'Check for nearby heat sources.'],
    ['Humidity', data.sht_status === 'READY' ? data.humidity : null, '% RH', 'rh_min', 'rh_max',
      'Check for dry airflow near the sensor.', 'Check for moisture sources and improve ventilation.']
  ];
  const alerts = [];
  let measured = 0;
  let message;
  if (state !== 'AWAKE' || data.mode !== 'AUTO') {
    message = 'Waiting for live AUTO readings.';
  } else if (!limits) {
    message = 'Save limits in Maintenance to enable recommendations.';
  } else {
    for (const [name, value, unit, min, max, lowTip, highTip] of readings) {
      if (!Number.isFinite(value)) continue;
      measured++;
      if (min && value < limits[min]) {
        alerts.push(`${name} is low (${value} ${unit}). Minimum: ${limits[min]} ${unit}. ${lowTip}`);
      } else if (value > limits[max]) {
        alerts.push(`${name} is high (${value} ${unit}). Maximum: ${limits[max]} ${unit}. ${highTip}`);
      }
    }
    if (alerts.length) {
      message = 'One or more readings are outside the saved limits.';
      if (data.fan_override === 'MANUAL') {
        alerts.push('The fan is in Manual. Adjust the knob or select AUTO for sensor-based extraction.');
      } else if (data.fan_override === 'OFF') {
        alerts.push('Fan override is OFF. Select Resume automatic to restore sensor-based extraction.');
      }
    } else if (measured === readings.length) {
      message = 'All five live readings are within the saved limits.';
    } else if (measured) {
      message = 'Available readings are within the saved limits. Some sensors have no fresh reading.';
    } else {
      message = 'Waiting for fresh sensor readings.';
    }
  }
  if (status.textContent !== message) status.textContent = message;
  const rows = alerts.map(message => {
    const item = document.createElement('li');
    item.textContent = message;
    return item;
  });
  list.replaceChildren(...rows);
  list.hidden = !alerts.length;
}

const resetSdButton=$('resetSdLog');
let resettingSd=false;
function updateSdResetButton(data) {
  if (!resetSdButton) return;
  const count=Number(data?.sd_log?.split(',')[1]);
  resetSdButton.disabled=resettingSd || !data?.connected || data.state!=='AWAKE' ||
    !data.sd_reset_supported || data.command_pending || data.sd_pending!==0 || !(count>0);
}
resetSdButton?.addEventListener('click',async()=>{
  if(!window.confirm('Reset FUMELOG.BIN so the SD card can log new readings? Saved online readings will stay.')) return;
  resettingSd=true;
  updateSdResetButton(lastDevice);
  $('resetSdStatus').textContent='Waiting for the device to confirm the reset…';
  try {
    const response=await fetch('/api/sd/reset',{
      method:'POST',
      headers:{'X-CSRF-Token':document.querySelector('meta[name=csrf-token]').content},
      signal:AbortSignal.timeout(16000)
    });
    const result=await response.json();
    if(!response.ok) throw new Error(result.error||'Reset could not be confirmed.');
    $('resetSdStatus').textContent=result.message;
  } catch(error) {
    $('resetSdStatus').textContent=error.message||'Could not confirm the SD reset.';
  } finally {
    resettingSd=false;
    scheduleStatus();
  }
});
function renderDevice(data){
  lastDevice=data;
  renderSystemStatusVisibility(data);
  updateSdResetButton(data);
  const sd = $('sdSyncStatus');
  if (!data.connected || data.state !== 'AWAKE') sd.textContent = 'SD sync waits for the device to be awake and reachable.';
  else if (data.sd_log === 'ERROR') sd.textContent = 'SD log unavailable. Check the card and FUMELOG.BIN, then restart.';
  else if (Number.isInteger(data.sd_pending)) {
    const count = Number(data.sd_log.split(',')[1]);
    sd.textContent = 'SD records: ' + count + '/4096 · ' + data.sd_pending + ' waiting to sync.';
    if (count === 4096) sd.textContent += ' Card log full. Archive it after sync and prepare a new file.';
  } else sd.textContent = 'Waiting for SD status.';
  const online=!!data.connected, state=online?data.state:'UNKNOWN';
  $('sessionPillText').textContent=online?'ESP connected':'Device offline';
  $('sessionDot').className='dot '+(online?'on':'off');
  $('espState').textContent=online?'Connected':'Offline';
  $('arduinoState').textContent=state;
  $('deviceState').textContent=state;
  const autoMode = online && state === 'AWAKE' && data.mode === 'AUTO';
  const override = data.fan_override;
  const manual = autoMode && override === 'MANUAL';
  $('deviceTitle').textContent=online?(state==='SLEEPING'?'Sleep acknowledged':state==='AWAKE'?(data.mode==='MAINTENANCE'?'Maintenance mode':'Arduino awake'):'Checking Arduino'):'Awaiting device';
  $('deviceMessage').textContent=online?(state==='SLEEPING'?'Sensor power-down acknowledged. Historical records remain available.':data.mode==='MAINTENANCE'?'Manual tests enabled. Automatic outputs and scheduled sensor logging are paused.':'Live sensor monitoring with automatic extraction.'):(data.message||'Cannot reach ESP. Check its power and Wi-Fi.');
  if (manual) {
    $('deviceTitle').textContent = 'MANUAL MODE';
    $('deviceMessage').textContent = 'Use D4 to switch the fan and the A0 knob to set its speed.';
  }
  const changedAt = $('sleepWakeChangedAt');
  changedAt.textContent = dateText(data.last_state_change);
  if (data.last_state_change) changedAt.dateTime = data.last_state_change;
  else changedAt.removeAttribute('datetime');
  $('changedAt').textContent = changedAt.textContent;
  const sensorStatus = {
    READY:'Connected', MISSING:'Missing', ERROR:'Read error', WARMING:'Warming up',
    CONDITIONING:'Conditioning', WAITING:'Waiting for data', STALE:'Stale',
    OFF:'Powered down', NOT_LINKED:'Firmware not linked'
  };
  for (const [id, key] of [['pmsSystem','pm_status'], ['sgpSystem','gas_status'], ['shtSystem','sht_status']]) {
    $(id).textContent = state === 'SLEEPING' ? 'Powered down' : state === 'AWAKE'
      ? sensorStatus[data[key]] || 'Waiting for data' : 'Unavailable';
  }
  renderRecommendations(data, state);
  for(const [id,text,ok] of [['espBadge',online?'Connected':'Offline',online],['arduinoBadge',state,state==='AWAKE'||state==='SLEEPING']]){
    $(id).textContent=text;$(id).className='conn-badge '+(ok?'online':'offline');
  }
  const fresh=online&&state==='AWAKE'&&Number.isFinite(data.temperature)&&Number.isFinite(data.humidity);
  reading('valTEMP',fresh?data.temperature:null,'°C');reading('valHUM',fresh?data.humidity:null,'% RH');
  $('sampleStatus').textContent=fresh?'Fresh SHT40 reading':state==='SLEEPING'?'Sensors powered down':'Waiting for a fresh SHT40 sample';
  const pmFresh=online&&state==='AWAKE'&&data.pm_status==='READY';
  reading('valPM',pmFresh?data.pm25:null,'µg/m³');
  const pmLabels={READY:'live atmospheric concentrations',WARMING:'warming up (30 s)',ERROR:'data error: check TX wiring',MISSING:'no frames: check power and TX',STALE:'readings stale',OFF:'powered down',WAITING:'waiting',NOT_LINKED:'firmware not linked'};
  $('pmStatus').textContent='PMS7003 · '+(!online?'device offline':state==='SLEEPING'?'powered down':pmLabels[data.pm_status]||'waiting');
  $('pmDetails').textContent='PM1.0: '+shown(pmFresh?data.pm1:null)+' · PM10: '+shown(pmFresh?data.pm10:null)+' µg/m³';
  const gasFresh=online&&state==='AWAKE'&&data.gas_status==='READY';
  reading('valVOC',gasFresh?data.voc_index:null,'idx');
  reading('valNOX',gasFresh?data.nox_index:null,'idx');
  const gasLabels={READY:'SGP41 · live index',WARMING:'SGP41 · warming up',CONDITIONING:'SGP41 · conditioning',ERROR:'SGP41 · error: check wiring, then sleep/wake',WAITING:'SGP41 · waiting for samples',STALE:'SGP41 · readings stale',OFF:'SGP41 · powered down',NOT_LINKED:'SGP41 · firmware not linked'};
  let gasText=!online?'SGP41 · device offline':state==='SLEEPING'?'SGP41 · powered down':gasLabels[data.gas_status]||'SGP41 · waiting for samples';
  if(gasFresh) gasText+=data.gas_compensation==='SHT40'?' · SHT40 compensated':' · default compensation';
  $('vocStatus').textContent=gasText;
  $('noxStatus').textContent=gasText;

  const pending=sending||!!data.command_pending||['WAKING','SLEEP_PENDING'].includes(state);
  const target=localTarget||data.command_target;
  const maintenance=online&&state==='AWAKE'&&data.mode==='MAINTENANCE';
  const confirmedMode = state === 'SLEEPING' ? 'SLEEP' : maintenance ? 'MAINTENANCE' : autoMode ? 'AUTO' : null;
  const requestedMode = pending ? (requestedModes[target] ??
    (state === 'SLEEP_PENDING' ? 'SLEEP' : state === 'WAKING' ? 'AUTO' : null)) : null;
  const displayedMode = requestedMode ?? confirmedMode;
  const buzzerReady=online&&state==='AWAKE'&&data.buzzer_supported;
  for(const id of ['buzzerPlay','buzzerStop','preparePowerOff']) $(id).disabled=!canControl||!buzzerReady||pending;
  $('buzzerTest').disabled=!canControl||!buzzerReady||!maintenance||pending;
  const buzzerStatus = buzzerReady ? 'Ready' : state === 'SLEEPING' ? 'Sleeping' : 'Unavailable';
  $('buzzerState').textContent = buzzerStatus;
  $('dashboardBuzzerState').textContent = buzzerStatus;

  const oledStatus = online ? ({ON: 'On', OFF: 'Off', MISSING: 'Missing',
    DISABLED: 'Disabled'})[data.oled] ?? 'Unknown' : 'Unknown';
  $('dashboardOledState').textContent = oledStatus;
  $('dashboardFanState').textContent = online ? ({ON: 'On (reported)', OFF: 'Off (reported)',
    PENDING: 'Waiting for confirmation'})[data.fan_relay] ?? 'Unknown' : 'Unknown';
  if(!sending && (data.command_target?.startsWith('BUZZER:') || data.command_message?.includes('buzzer'))) {
    $('buzzerFeedback').textContent=data.command_message||'';
  }
  const extraction = $('extractionStatus');
  $('autoFanControls').hidden = !autoMode || manual;
  const overrideReady = autoMode && !manual && canControl && !pending && !!override && data.fan_relay !== 'PENDING';
  $('autoFanOn').disabled = !overrideReady || override === 'ON';
  $('autoFanOff').disabled = !overrideReady || override === 'OFF';
  $('autoFanResume').disabled = !overrideReady || override === 'AUTO';
  if (data.command_target?.startsWith('FAN_OVERRIDE:')) {
    $('autoFanFeedback').textContent = 'Waiting for Arduino confirmation…';
  } else if (autoMode && !override) {
    $('autoFanFeedback').textContent = 'Waiting for fan override status. Update both device sketches if this persists.';
  } else if (data.command_message?.startsWith('Fan override') ||
             data.command_message === 'Automatic fan control resumed.') {
    $('autoFanFeedback').textContent = data.command_message;
  } else $('autoFanFeedback').textContent = '';
  if (!online || state !== 'AWAKE') extraction.textContent = 'Waiting for awake device.';
  else if (maintenance) extraction.textContent = 'Paused in Maintenance.';
  else if (data.mode !== 'AUTO') extraction.textContent = 'Waiting for AUTO confirmation.';
  else if (manual) extraction.textContent = 'Manual · A0 knob · ' +
    (data.fan_relay === 'OFF' ? 'fan off' : data.fan_relay === 'ON' ? shown(data.fan_duty) + '% PWM' : 'waiting for fan status');
  else if (override === 'ON') {
    const duty = Number.isInteger(data.fan_duty) && data.fan_relay === 'ON'
      ? ' at ' + data.fan_duty + '% PWM' : '';
    const adaptive = data.threshold_sync === 'applied' && (pmFresh || gasFresh);
    extraction.textContent = duty
      ? 'Fan held ON' + duty + (adaptive ? ' · sensor-controlled speed.' : ' · base speed until readings and limits are ready.')
      : 'Fan ON requested. Waiting for fan status.';
  }
  else if (override === 'OFF') extraction.textContent = 'Fan manually OFF. Automatic extraction is paused.';
  else if (data.threshold_sync !== 'applied') extraction.textContent = 'Save and confirm your limits in Maintenance to enable AUTO.';
  else if (data.fan_relay === 'ON') extraction.textContent = 'Running at ' + shown(data.fan_duty) + '% PWM.';
  else if (!pmFresh && !gasFresh) extraction.textContent = 'Waiting for a valid PM2.5 or gas reading.';
  else extraction.textContent = 'Monitoring. Fan is off.';
  $('modeAuto').checked = displayedMode === 'AUTO';
  $('autoModeDetail').textContent = manual ? 'Manual · speed set by A0 knob' : 'Monitor sensors';
  $('modeSleep').checked = displayedMode === 'SLEEP';
  $('modeMaintenance').checked = displayedMode === 'MAINTENANCE';
  $('modeMaintenance').disabled=!canControl||!online||pending||state!=='AWAKE';
  $('sensorCheckPanel').hidden=!maintenance||currentView!=='overview';
  renderThresholds(data, maintenance, pending);
  $('checkSensors').disabled=!canControl||!maintenance||pending;
  for(const id of ['oledOn','oledOff']) $(id).disabled=!canControl||!maintenance||pending;
  $('oledReported').textContent=online?(data.oled||'Unknown'):'Unknown';
  $('oledSystem').textContent=online?(data.oled||'Unknown'):'Unknown';
  $('modeAuto').disabled=!canControl||!online||pending;
  $('modeSleep').disabled=!canControl||!online||pending||state!=='AWAKE';
  $('wakeArduino').disabled = !canControl || !online || pending || state === 'AWAKE';
  $('modePicker').setAttribute('aria-busy',String(pending));
  $('confirmedMode').textContent = requestedMode ? `Switching to ${requestedMode}…`
    : confirmedMode ? 'Confirmed: ' + (manual ? 'AUTO · Manual' : confirmedMode)
    : 'Mode unconfirmed';
  $('fanResult').textContent=sending&&localTarget?.startsWith('FAN_')?'Sending fan request…':data.command_target?.startsWith('FAN_')?'Waiting for Arduino fan acknowledgement…':data.command_message||'';
  const fanReady=canControl&&maintenance&&state==='AWAKE'&&!pending&&data.fan_supported&&data.fan_relay!=='PENDING';
  const speedReady=fanReady&&Number.isInteger(data.fan_duty);
  for(const id of ['fanSpeed','fanApplySpeed']) $(id).disabled=!speedReady;
  $('fanDuty').textContent=online&&Number.isInteger(data.fan_duty)?data.fan_duty+'%':'—';
  const rpm=online&&state==='AWAKE'&&data.fan_relay==='ON'&&Number.isInteger(data.fan_rpm)?data.fan_rpm:null;
  $('fanRpm').textContent=rpm===null?'—':rpm+' RPM';
  $('valFAN').textContent=rpm===null?'— RPM':rpm+' RPM';
  $('fanSpeedStatus').textContent=online&&Number.isInteger(data.fan_duty)?(maintenance?'Choose a PWM duty and click Apply speed.':'Fan duty is set automatically in AUTO.'):'Waiting for PWM firmware status.';
  $('fanOnButton').disabled=!fanReady;
  $('fanOffButton').disabled=!fanReady;
  $('fanRelay').textContent=online?((data.fan_relay||'UNKNOWN')+' (reported)'):'Unknown';
  $('fanHelp').textContent=!data.fan_supported?'Upload both fan-test sketches to enable these buttons.':state!=='AWAKE'?'Select Wake Arduino.':maintenance?'D10 switches power. D6 sets the PWM duty.':'Select MAINTENANCE to test the fan and OLED.';
  if(!sending){
    const rejection = rejectedCommand && Date.now() < rejectedCommand.until
      ? rejectedCommand.message : '';
    $('commandResult').textContent = (pending ? '' : rejection) ||
      (!online ? 'Device unavailable. Check ESP power and Wi-Fi.' :
      data.command_message || (pending ? 'Waiting for Arduino acknowledgement…' :
      !canControl?'Viewer access: mode controls are read-only.':
      state==='AWAKE'?'Arduino awake. Select SLEEP to power down sensors.':
      state==='SLEEPING'?'Arduino asleep. Select Wake Arduino to return to AUTO.':'Arduino state unknown. Select Wake Arduino to send a wake request.'));
  }
}
async function command(action,value){
  if(!canControl||sending) return;
  sending = true;
  rejectedCommand = null;
  localTarget = modeTargets[action] ?? (action.startsWith('fan_') ? action.toUpperCase() : null);
  statusEpoch++;
  renderDevice(lastDevice||{connected:false,state:'UNKNOWN'});
  $('commandResult').textContent='Sending '+action.replace('_',' ')+' request…';
  let message, snapshot;
  try{
    const response=await fetch('/api/command',{method:'POST',headers:{'Content-Type':'application/json','X-CSRF-Token':document.querySelector('meta[name=csrf-token]').content},body:JSON.stringify({action,value}),signal:AbortSignal.timeout(10000)});
    const data=await response.json();
    message=response.ok?data.message:data.error;
    snapshot=data.device;
    if (response.status >= 400 && response.status < 500 && message) {
      rejectedCommand = {message, until: Date.now() + 8000};
    }
  }catch{message='Request interrupted. Checking Arduino state; no automatic retry was sent.';}
  finally{
    sending=false;localTarget=null;statusEpoch++;
    renderDevice(snapshot||lastDevice||{connected:false,state:'UNKNOWN'});
    $('commandResult').textContent=message||'Checking Arduino state…';
    if(action === 'fan_override') $('autoFanFeedback').textContent=message||'Checking fan status…';
    else if(action.startsWith('fan_')) $('fanResult').textContent=message||'Checking fan status…';
    if(action==='buzzer') $('buzzerFeedback').textContent=message||'Checking Arduino state…';
    scheduleStatus();
  }
}
$('buzzerPlay').addEventListener('click',()=>command('buzzer',$('buzzerSong').value));
$('buzzerStop').addEventListener('click',()=>command('buzzer','stop'));
$('buzzerTest').addEventListener('click',()=>command('buzzer','test'));
$('preparePowerOff').addEventListener('click',()=>command('shutdown'));
$('modeAuto').addEventListener('change',()=>command(lastDevice?.state==='AWAKE'?'mode_auto':'wake'));
$('modeAuto').addEventListener('click',()=>{
  if (lastDevice?.fan_override === 'MANUAL') command('mode_auto');
});
const thresholdForm = $('thresholdForm');
const thresholdFields = [...thresholdForm.querySelectorAll('input')];
let thresholdsLoaded = false;
let thresholdsBusy = false;

function renderThresholds(data, maintenance, pending) {
  $('thresholdPanel').hidden = !maintenance || currentView !== 'overview';
  const editable = canControl && maintenance && !pending && !thresholdsBusy && thresholdsLoaded;
  for (const id of ['thresholdInputs', 'thresholdPreset', 'thresholdSave']) $(id).disabled = !editable;
  const messages = {
    unset: 'No limits saved yet.',
    applied: 'Arduino confirmed the saved limits.',
    pending: 'Saved limits are waiting for Arduino confirmation.'
  };
  $('thresholdSync').textContent = messages[data.threshold_sync] || '';
}

async function loadThresholds() {
  try {
    const {values} = await getJSON('/api/thresholds');
    for (const field of thresholdFields) field.value = values?.[field.name] ?? '';
    thresholdsLoaded = true;
  } catch (error) {
    $('thresholdFeedback').textContent = error.message;
  }
}

$('thresholdPreset').addEventListener('click', () => {
  for (const field of thresholdFields) field.value = field.dataset.suggested;
  $('thresholdFeedback').textContent = 'Review these values, then save to apply them.';
});

thresholdForm.addEventListener('submit', async event => {
  event.preventDefault();
  if ($('thresholdSave').disabled) return;
  const values = Object.fromEntries(thresholdFields.map(field => [field.name, field.valueAsNumber]));
  thresholdsBusy = true;
  renderThresholds(lastDevice, true, false);
  try {
    const response = await fetch('/api/thresholds', {
      method: 'POST',
      headers: {'Content-Type': 'application/json', 'X-CSRF-Token': document.querySelector('meta[name=csrf-token]').content},
      body: JSON.stringify(values),
      signal: AbortSignal.timeout(8000)
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || 'Could not save thresholds.');
    $('thresholdFeedback').textContent = result.message;
    $('thresholdSync').textContent = '';
  } catch (error) {
    $('thresholdFeedback').textContent = error.message;
  } finally {
    thresholdsBusy = false;
    renderThresholds(lastDevice, lastDevice?.mode === 'MAINTENANCE', lastDevice?.command_pending);
  }
});
loadThresholds();
$('modeMaintenance').addEventListener('change',()=>command('mode_maintenance'));
$('oledOn').addEventListener('click',()=>command('oled_on'));
$('oledOff').addEventListener('click',()=>command('oled_off'));
$('checkSensors').addEventListener('click',async()=>{
  $('checkSensors').disabled=true;
  $('checkReadings').replaceChildren();
  $('checkStatus').textContent='Requesting readings…';
  try {
    const response=await fetch('/api/sensor-check',{method:'POST',headers:{'X-CSRF-Token':document.querySelector('meta[name=csrf-token]').content},signal:AbortSignal.timeout(10000)});
    const result=await response.json();
    if(!response.ok) throw new Error(result.error||'Check failed.');
    const d=result.device;
    const value=(n,u)=>Number.isFinite(n)?n.toFixed(2)+' '+u:'Unavailable';
    const rows=[['SHT40',value(d.temperature,'°C')+' / '+value(d.humidity,'% RH'),d.temperature==null?'No fresh sample':'Sample age: '+d.sample_age+' s'],
      ['SGP41','VOC '+value(d.voc_index,'idx')+' / NOx '+value(d.nox_index,'idx'),d.gas_status],
      ['PMS7003','PM1 '+value(d.pm1,'µg/m³')+' / PM2.5 '+value(d.pm25,'µg/m³')+' / PM10 '+value(d.pm10,'µg/m³'),d.pm_status],
      ['Fan tachometer',value(d.fan_rpm,'RPM'),d.fan_relay==='ON'?'Fan commanded ON':'Turn fan ON to test tachometer']];
    for(const row of rows){const tr=document.createElement('tr');for(const text of row){const td=document.createElement('td');td.textContent=text;tr.append(td);}$('checkReadings').append(tr);}
    $('checkStatus').textContent='Checked '+dateText(result.checked_at)+'. Latest available samples; warmup and missing readings remain marked.';
  } catch(error){$('checkStatus').textContent=error.message||'Could not read sensors.';}
  finally {renderDevice(lastDevice||{connected:false});}
});
$('modeSleep').addEventListener('change',()=>command('sleep'));
$('wakeArduino').addEventListener('click',()=>command('wake'));
$('autoFanOn').addEventListener('click',()=>command('fan_override','ON'));
$('autoFanOff').addEventListener('click',()=>command('fan_override','OFF'));
$('autoFanResume').addEventListener('click',()=>command('fan_override','AUTO'));
$('fanOnButton').addEventListener('click',()=>command('fan_on'));
$('fanOffButton').addEventListener('click',()=>command('fan_off'));
$('fanSpeed').addEventListener('input',()=>{$('fanSpeedValue').textContent=$('fanSpeed').value+'%';});
$('fanApplySpeed').addEventListener('click',()=>command('fan_speed',Number($('fanSpeed').value)));
function clock(){$('clock').textContent=dateText(new Date().toISOString())+' · Philippine time';}
showView(location.hash.slice(1));clock();setInterval(clock,30000);poll();setTimeout(refreshHistory,60000);
