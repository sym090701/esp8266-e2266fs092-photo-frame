#include <Arduino.h>
#include <DNSServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <SPI.h>

#include "secrets.h"

namespace {

constexpr uint8_t PIN_BUSY = 5;   // D1
constexpr uint8_t PIN_RESET = 2;  // D4
constexpr uint8_t PIN_DC = 4;     // D2
constexpr uint8_t PIN_CS = 15;    // D8
constexpr uint8_t PIN_BUTTON = 0; // FLASH button

constexpr uint16_t DISPLAY_WIDTH = 152;
constexpr uint16_t DISPLAY_HEIGHT = 296;
constexpr uint16_t NATIVE_WIDTH = 152;
constexpr uint16_t NATIVE_HEIGHT = 296;
constexpr size_t ROW_BYTES = NATIVE_WIDTH / 8;
constexpr size_t FRAME_BYTES = ROW_BYTES * NATIVE_HEIGHT;
constexpr size_t UPLOAD_BYTES = FRAME_BYTES * 2;
constexpr uint32_t BUSY_TIMEOUT_MS = 60000;
constexpr uint32_t WIFI_TIMEOUT_MS = 20000;
constexpr uint32_t BUTTON_IP_PRESS_MS = 1500;
constexpr uint32_t BUTTON_CLEAR_PRESS_MS = 5000;
constexpr uint16_t TCP_CONTROL_PORT = 8266;
constexpr char MDNS_HOSTNAME[] = "epd-photo";

constexpr char PHOTO_PATH[] = "/photo.bin";
constexpr char PHOTO_TEMP_PATH[] = "/photo.tmp";
constexpr char SHORTCUT_SOURCE_PATH[] = "/shortcut.jpg";
constexpr char SHORTCUT_SOURCE_TEMP_PATH[] = "/shortcut.tmp";
constexpr char SCREEN_MARKER_PATH[] = "/epd-photo-v2";
constexpr char FIRMWARE_VERSION[] =
    "photo-api-ota-shortcut-4-compare-calibration-mdns";
constexpr size_t TCP_COMMAND_MAX_BYTES = 80;
constexpr size_t SHORTCUT_SOURCE_MAX_BYTES = 850 * 1024;

uint8_t blackFrame[FRAME_BYTES];
uint8_t redFrame[FRAME_BYTES];

DNSServer dnsServer;
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
WiFiServer tcpServer(TCP_CONTROL_PORT);
WiFiClient tcpClient;
String tcpCommand;
File shortcutSourceUpload;
size_t shortcutSourceBytes = 0;
bool shortcutSourceUploadValid = false;

size_t uploadOffset = 0;
bool uploadValid = false;
bool refreshQueued = false;
bool storedPhotoQueued = false;
bool ipScreenQueued = false;
bool clearScreenQueued = false;
bool fileSystemReady = false;
bool accessPointMode = false;
bool mdnsReady = false;
uint32_t lastRefreshMs = 0;
bool buttonRawState = HIGH;
bool buttonStableState = HIGH;
bool buttonWasPressed = false;
bool buttonIpIndicated = false;
bool buttonClearIndicated = false;
uint32_t buttonChangedAt = 0;
uint32_t buttonPressedAt = 0;

enum class DisplayState : uint8_t { Idle, Queued, Refreshing, Done, Error };
DisplayState displayState = DisplayState::Idle;

struct Glyph {
  char code;
  uint8_t columns[5];
};

constexpr Glyph FONT[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'N', {0x7F, 0x02, 0x0C, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
};

enum class Ink : uint8_t { White, Black, Red };

const char PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>墨水屏照片</title>
<style>
:root{color-scheme:light;--ink:#182021;--muted:#687273;--line:#d7dddd;--line-strong:#b9c4c4;--red:#bd302f;--red-soft:#fff3f1;--paper:#ffffff;--bg:#f2f5f4;--stage:#e5ebea}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;letter-spacing:0}
main{width:min(1020px,100%);margin:0 auto;padding:28px 20px 48px}
header{display:flex;align-items:center;justify-content:space-between;gap:20px;padding:0 2px 18px;border-bottom:1px solid var(--line);margin-bottom:22px}
h1{font-size:26px;line-height:1.2;margin:0;font-weight:750}
.header-status{display:grid;justify-items:end;gap:4px;min-width:0}
#status{font-size:13px;color:var(--muted);text-align:right;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:270px}
#deviceAddress{font-size:13px;color:var(--red);text-decoration:none;font-weight:650}
#deviceAddress:hover{text-decoration:underline}
.workspace{display:grid;grid-template-columns:minmax(310px,390px) minmax(340px,1fr);gap:24px;align-items:start}
.preview,.controls{background:var(--paper);border:1px solid var(--line);border-radius:8px;box-shadow:0 10px 24px #1520200a}
.preview{overflow:hidden}
.preview-bar{display:flex;align-items:center;justify-content:space-between;min-height:50px;padding:8px 12px;border-bottom:1px solid var(--line);font-size:14px;font-weight:720}
.zoom-tools{display:flex;align-items:center;gap:5px}
.zoom-tools button{width:32px;min-height:32px;padding:0;font-size:18px;line-height:1}
#zoomValue{width:48px;text-align:center;color:var(--muted);font-size:12px;font-weight:650;font-variant-numeric:tabular-nums}
.stage{display:grid;place-items:center;min-height:76mm;padding:20px;overflow:hidden;background:var(--stage);background-image:linear-gradient(45deg,#dce5e3 25%,transparent 25%),linear-gradient(-45deg,#dce5e3 25%,transparent 25%),linear-gradient(45deg,transparent 75%,#dce5e3 75%),linear-gradient(-45deg,transparent 75%,#dce5e3 75%);background-size:16px 16px;background-position:0 0,0 8px,8px -8px,-8px 0}
.screen{box-sizing:content-box;width:31mm;padding:4px;background:#1b2222;border:1px solid #0b1010;box-shadow:0 8px 18px #1520203d}
canvas{display:block;width:100%;height:auto;aspect-ratio:152/296;image-rendering:pixelated;background:#fff;cursor:grab;touch-action:none;user-select:none}
canvas.dragging{cursor:grabbing}
.controls{display:grid;gap:0;padding:0;overflow:hidden}
.control-section{display:grid;gap:14px;padding:18px}
.control-section+.control-section{border-top:1px solid var(--line)}
.section-title{margin:0;color:var(--muted);font-size:11px;font-weight:760;line-height:1;text-transform:uppercase}
label{display:grid;gap:7px;font-size:13px;font-weight:700}
label:has(input:disabled){opacity:.42}
[hidden]{display:none!important}
input[type=file]{width:100%;padding:8px;border:1px dashed var(--line-strong);border-radius:6px;background:#f8faf9;color:var(--muted);font:inherit;font-size:12px}
input[type=range]{width:100%;accent-color:var(--red);cursor:pointer}
select{width:100%;min-height:42px;border:1px solid var(--line-strong);border-radius:6px;background:#fff;padding:8px 10px;color:var(--ink);font:inherit;font-size:14px}
.row{display:flex;justify-content:space-between;color:var(--muted);font-size:12px;font-weight:500}
.segments{display:grid;grid-template-columns:1fr 1fr;border:1px solid var(--line-strong);border-radius:6px;overflow:hidden;background:#fff}
.segments label{display:flex;align-items:center;justify-content:center;min-height:42px;padding:8px;text-align:center;cursor:pointer;font-size:13px;font-weight:700}
.segments label+label{border-left:1px solid var(--line)}
.segments input{position:absolute;opacity:0;pointer-events:none}
.segments label:has(input:checked){background:var(--ink);color:#fff}
button{border:1px solid var(--ink);background:var(--ink);color:#fff;min-height:42px;padding:9px 14px;font:inherit;font-size:13px;font-weight:720;cursor:pointer;border-radius:6px;transition:background .16s,border-color .16s,transform .16s}
button:hover:not(:disabled){background:#2c3738;border-color:#2c3738}
button:active:not(:disabled){transform:translateY(1px)}
button:focus-visible,select:focus-visible,input:focus-visible{outline:3px solid #bd302f38;outline-offset:2px}
button:disabled{background:#aeb7b7;border-color:#aeb7b7;cursor:not-allowed}
.comparison{min-width:0}
.comparison-title{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:8px;font-size:13px;font-weight:720}
#compareStatus{color:var(--muted);font-size:12px;font-weight:500;white-space:nowrap}
.comparison-list{display:flex;gap:8px;overflow-x:auto;padding:0 0 4px;scroll-snap-type:x proximity;scrollbar-color:var(--line-strong) transparent}
.compare-option{display:grid;place-items:center;gap:5px;flex:0 0 92px;min-height:136px;padding:8px 6px;background:#fff;color:var(--ink);border:1px solid var(--line);border-radius:6px;font-size:11px;font-weight:650;line-height:1.25;scroll-snap-align:start}
.compare-option:hover{background:#f8faf9;border-color:var(--line-strong)}
.compare-option[aria-pressed=true]{border:2px solid var(--red);padding:7px 5px;color:var(--red);background:var(--red-soft)}
.compare-option canvas{width:54px;margin:0;border:1px solid var(--line);cursor:pointer;touch-action:auto}
.tool-grid,.calibration-settings{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.calibration-settings{gap:14px;padding-top:2px}
.calibration-settings button{grid-column:1/-1}
.manual-settings{display:grid;grid-template-columns:1fr 1fr;gap:14px}
.manual-settings label:last-child{grid-column:1/-1}
.actions{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.upload-dock{padding:14px 18px 18px;border-top:1px solid var(--line);background:#fbfcfc}
.upload-dock button{width:100%;min-height:48px;background:var(--red);border-color:var(--red);font-size:14px}
.upload-dock button:hover:not(:disabled){background:#a82424;border-color:#a82424}
button.secondary{background:#fff;color:var(--ink);border-color:var(--line-strong)}
button.secondary:hover:not(:disabled){background:#eef3f2;border-color:#9faaaa}
button.danger{background:#fff;color:var(--red);border-color:#e4a3a0}
button.danger:hover:not(:disabled){background:var(--red-soft);border-color:var(--red)}
.red{color:var(--red)}
@media(max-width:760px){main{padding:18px 14px 28px}header{align-items:flex-start;margin-bottom:16px;padding-bottom:14px}h1{font-size:23px}.header-status{max-width:52%;padding-top:2px}.workspace{grid-template-columns:minmax(0,560px);gap:16px}.stage{min-height:74mm;padding:18px}.controls{overflow:visible}.control-section{padding:16px}.manual-settings{grid-template-columns:1fr}.manual-settings label:last-child{grid-column:auto}.upload-dock{position:sticky;bottom:10px;z-index:4;margin-top:0;padding:10px 12px;border:1px solid var(--line);border-radius:8px;box-shadow:0 8px 20px #15202024}.comparison-list{margin-right:-2px}.compare-option{flex-basis:88px}}
@media(max-width:390px){main{padding-inline:10px}.stage{padding:14px}.tool-grid,.calibration-settings,.actions{grid-template-columns:1fr}.screen{width:29mm}}
</style>
</head>
<body>
<main>
<header><h1>墨水屏照片</h1><div class="header-status"><div id="status">请选择照片</div><a id="deviceAddress" href="http://epd-photo.local/">epd-photo.local</a></div></header>
<div class="workspace">
  <div class="preview">
    <div class="preview-bar"><span>预览</span><div class="zoom-tools"><button id="zoomOut" class="secondary" type="button" title="缩小" aria-label="缩小">−</button><output id="zoomValue">100%</output><button id="resetCrop" class="secondary" type="button" title="居中复位" aria-label="居中复位">↺</button><button id="zoomIn" class="secondary" type="button" title="放大" aria-label="放大">+</button></div></div>
    <div class="stage"><div class="screen"><canvas id="preview" width="152" height="296"></canvas></div></div>
  </div>
  <div class="controls">
    <section class="control-section">
      <p class="section-title">照片与色彩</p>
      <label class="file">选择照片<input id="file" type="file" accept="image/*"></label>
      <div class="segments">
        <label><input type="radio" name="mode" value="tri" checked>黑白红</label>
        <label><input type="radio" name="mode" value="mono">仅黑白</label>
      </div>
    </section>
    <section class="control-section">
      <p class="section-title">构图</p>
      <label class="ratio">画面比例
        <select id="ratio">
          <option value="screen">全屏</option>
          <option value="source">原图比例</option>
          <option value="1">方形 1:1</option>
          <option value="0.75">竖版 3:4</option>
          <option value="0.6666667">竖版 2:3</option>
          <option value="0.5625">竖版 9:16</option>
        </select>
      </label>
      <div class="segments">
        <label><input type="radio" name="fit" value="crop" checked>裁剪填满</label>
        <label><input type="radio" name="fit" value="contain">完整显示留白</label>
      </div>
      <label class="preview-size">预览尺寸校准 <span class="row"><span>小</span><output id="previewScaleValue">100%</output><span>大</span></span><input id="previewScale" type="range" min="60" max="180" value="100"></label>
      <div class="tool-grid">
        <button id="rotate" class="secondary" type="button" title="顺时针旋转照片">旋转 90 度</button>
        <button id="calibrationTarget" class="secondary" type="button" title="显示屏幕校准图">显示校准图</button>
      </div>
    </section>
    <section class="control-section">
      <p class="section-title">图像算法</p>
      <label class="algorithm">抖动与量化方式
        <select id="algorithm">
          <option value="waveshare">微雪官方抖动（默认）</option>
          <option value="floyd">Floyd–Steinberg（最初版）</option>
          <option value="tri-floyd">三色 FS（最初版）</option>
          <option value="auto-photo">黑白块面（实验）</option>
        </select>
      </label>
      <div class="comparison">
      <div class="comparison-title"><span>四种效果对比</span><span id="compareStatus">选择照片后生成</span></div>
      <div class="comparison-list" id="algorithmCompare">
        <button class="compare-option" type="button" data-algorithm="waveshare" aria-pressed="true"><canvas width="152" height="296"></canvas><span>微雪官方</span></button>
        <button class="compare-option" type="button" data-algorithm="floyd" aria-pressed="false"><canvas width="152" height="296"></canvas><span>最初 FS</span></button>
        <button class="compare-option" type="button" data-algorithm="tri-floyd" aria-pressed="false"><canvas width="152" height="296"></canvas><span>三色 FS</span></button>
        <button class="compare-option" type="button" data-algorithm="auto-photo" aria-pressed="false"><canvas width="152" height="296"></canvas><span>黑白块面</span></button>
      </div>
      </div>
    </section>
    <section class="control-section">
      <p class="section-title">屏幕校准与手动调整</p>
      <div class="calibration-settings">
        <label>黑色校准 <span class="row"><span>少黑</span><output id="blackOffsetValue">0</output><span>多黑</span></span><input id="blackOffset" type="range" min="-30" max="30" value="0"></label>
        <label class="red">红色校准 <span class="row"><span>少红</span><output id="redOffsetValue">0</output><span>多红</span></span><input id="redOffset" type="range" min="-30" max="30" value="0"></label>
        <button id="resetCalibration" class="secondary" type="button">恢复本屏默认值</button>
      </div>
      <div class="manual-settings">
        <label class="manual-detail">亮度 <span class="row"><span>暗</span><output id="brightnessValue">0</output><span>亮</span></span><input id="brightness" type="range" min="-80" max="80" value="0"></label>
        <label class="manual-detail">对比度 <span class="row"><span>柔和</span><output id="contrastValue">20</output><span>强烈</span></span><input id="contrast" type="range" min="-40" max="80" value="20"></label>
        <label class="manual-detail">锐化 <span class="row"><span>自然</span><output id="sharpnessValue">80</output><span>清晰</span></span><input id="sharpness" type="range" min="0" max="180" value="80"></label>
        <label class="manual-detail">边缘保留降噪 <span class="row"><span>保留纹理</span><output id="denoiseValue">0</output><span>更平滑</span></span><input id="denoise" type="range" min="0" max="100" value="0"></label>
        <label class="manual-detail">黑场阈值 <span class="row"><span>少黑</span><output id="blackPointValue">145</output><span>多黑</span></span><input id="blackPoint" type="range" min="80" max="200" value="145"></label>
        <label class="manual-detail red">红色灵敏度 <span class="row"><span>低</span><output id="redLevelValue">50</output><span>高</span></span><input id="redLevel" type="range" min="0" max="100" value="50"></label>
      </div>
      <div class="actions">
        <button id="clear" class="danger" type="button">清空屏幕</button>
        <button id="ota" class="secondary" type="button">固件 OTA 更新</button>
      </div>
    </section>
    <div class="upload-dock"><button id="upload" disabled>上传到墨水屏</button></div>
  </div>
</div>
</main>
<script>
const W=152,H=296,RB=19,FB=5624;
const file=document.querySelector('#file');
const canvas=document.querySelector('#preview');
const ctx=canvas.getContext('2d',{willReadFrequently:true});
const status=document.querySelector('#status');
const deviceAddress=document.querySelector('#deviceAddress');
const upload=document.querySelector('#upload');
const rotateButton=document.querySelector('#rotate');
const clearButton=document.querySelector('#clear');
const zoomValue=document.querySelector('#zoomValue');
const screen=document.querySelector('.screen');
const previewScale=document.querySelector('#previewScale');
const previewScaleValue=document.querySelector('#previewScaleValue');
const zoomButtons=['zoomOut','resetCrop','zoomIn'].map(id=>document.querySelector('#'+id));
const controls=['brightness','contrast','sharpness','denoise','blackPoint','redLevel'];
const algorithmSelect=document.querySelector('#algorithm');
const compareStatus=document.querySelector('#compareStatus');
const compareButtons=[...document.querySelectorAll('.compare-option')];
const calibrationTarget=document.querySelector('#calibrationTarget');
const scratch=document.createElement('canvas');
const pointers=new Map();
let source=null,photoSource=null,payload=null,rotation=0,renderPending=false,compareTimer=0,gesture=null,safariGestureZoom=1;
let calibrationMode=false;
let edit={zoom:1,x:0,y:0};
rotateButton.disabled=true;
zoomButtons.forEach(button=>button.disabled=true);

function value(id){return Number(document.querySelector('#'+id).value)}
function clamp(v){return Math.max(0,Math.min(255,v))}
function clampValue(v,min,max){return Math.max(min,Math.min(max,v))}
function calibratedValue(id){
  const input=document.querySelector('#'+id);
  const offset=id==='blackPoint'?value('blackOffset'):id==='redLevel'?value('redOffset'):0;
  return clampValue(Number(input.value)+offset,Number(input.min),Number(input.max));
}
function loadCalibrationProfile(){
  try{
    const saved=JSON.parse(localStorage.getItem('epdPanelProfile')||'{}');
    if(Number.isFinite(saved.blackOffset))document.querySelector('#blackOffset').value=clampValue(saved.blackOffset,-30,30);
    if(Number.isFinite(saved.redOffset))document.querySelector('#redOffset').value=clampValue(saved.redOffset,-30,30);
  }catch(error){}
  document.querySelector('#blackOffsetValue').value=value('blackOffset');
  document.querySelector('#redOffsetValue').value=value('redOffset');
}
function saveCalibrationProfile(){
  document.querySelector('#blackOffsetValue').value=value('blackOffset');
  document.querySelector('#redOffsetValue').value=value('redOffset');
  try{localStorage.setItem('epdPanelProfile',JSON.stringify({blackOffset:value('blackOffset'),redOffset:value('redOffset')}))}catch(error){}
  scheduleRender();
}
loadCalibrationProfile();
function applyPreviewScale(save=true){
  const scale=value('previewScale');screen.style.width=31*scale/100+'mm';previewScaleValue.value=scale+'%';
  if(save)try{localStorage.setItem('epdPreviewScale',scale)}catch(error){}
}
try{
  const saved=Number(localStorage.getItem('epdPreviewScale'));
  if(saved>=60&&saved<=180)previewScale.value=saved;
}catch(error){}
applyPreviewScale(false);
function updateDetailControls(){
  const algorithm=algorithmSelect.value;
  const automatic=algorithm==='auto-photo'||algorithm==='waveshare';
  controls.forEach(id=>{
    const input=document.querySelector('#'+id);
    input.disabled=automatic;
    input.closest('label').hidden=automatic;
  });
}
function orientedSize(image){
  const iw=image.naturalWidth,ih=image.naturalHeight;
  return rotation%180?{iw,ih,ow:ih,oh:iw}:{iw,ih,ow:iw,oh:ih};
}
function photoFrame(ow,oh){
  const setting=document.querySelector('#ratio').value;
  if(setting==='screen')return{x:0,y:0,w:W,h:H};
  const ratio=setting==='source'?ow/oh:Number(setting);
  let w=W,h=Math.round(W/ratio);
  if(h>H){h=H;w=Math.round(H*ratio)}
  return{x:Math.floor((W-w)/2),y:Math.floor((H-h)/2),w,h};
}
function geometry(){
  const size=orientedSize(source);
  const frame=photoFrame(size.ow,size.oh);
  const crop=document.querySelector('input[name=fit]:checked').value==='crop';
  const base=(crop?Math.max:Math.min)(frame.w/size.ow,frame.h/size.oh);
  return{size,frame,crop,dw:size.ow*base*edit.zoom,dh:size.oh*base*edit.zoom,scale:base*edit.zoom};
}
function clampEdit(g=geometry()){
  if(g.crop){
    edit.x=clampValue(edit.x,-Math.max(0,(g.dw-g.frame.w)/2),Math.max(0,(g.dw-g.frame.w)/2));
    edit.y=clampValue(edit.y,-Math.max(0,(g.dh-g.frame.h)/2),Math.max(0,(g.dh-g.frame.h)/2));
  }else{
    edit.x=clampValue(edit.x,-(g.frame.w+g.dw)/2+8,(g.frame.w+g.dw)/2-8);
    edit.y=clampValue(edit.y,-(g.frame.h+g.dh)/2+8,(g.frame.h+g.dh)/2-8);
  }
}
function updateZoom(){zoomValue.value=Math.round(edit.zoom*100)+'%'}
function resetEdit(){edit={zoom:1,x:0,y:0};updateZoom()}
function setZoom(next,anchorX=0,anchorY=0){
  if(!source)return;
  const min=document.querySelector('input[name=fit]:checked').value==='crop'?1:.35;
  next=clampValue(next,min,6);
  const ratio=next/edit.zoom;
  edit.x=anchorX-(anchorX-edit.x)*ratio;
  edit.y=anchorY-(anchorY-edit.y)*ratio;
  edit.zoom=next;clampEdit();updateZoom();scheduleRender();
}
function drawPhoto(image){
  ctx.fillStyle='#fff';ctx.fillRect(0,0,W,H);
  const g=geometry();clampEdit(g);
  scratch.width=Math.max(1,Math.ceil(g.dw));scratch.height=Math.max(1,Math.ceil(g.dh));
  const sctx=scratch.getContext('2d');
  sctx.imageSmoothingEnabled=true;sctx.imageSmoothingQuality='high';
  sctx.translate(scratch.width/2,scratch.height/2);
  sctx.rotate(rotation*Math.PI/180);
  sctx.drawImage(image,-g.size.iw*g.scale/2,-g.size.ih*g.scale/2,g.size.iw*g.scale,g.size.ih*g.scale);
  ctx.save();ctx.beginPath();ctx.rect(g.frame.x,g.frame.y,g.frame.w,g.frame.h);ctx.clip();
  ctx.drawImage(scratch,g.frame.x+g.frame.w/2+edit.x-g.dw/2,g.frame.y+g.frame.h/2+edit.y-g.dh/2,g.dw,g.dh);
  ctx.restore();
}
function percentile(hist,total,fraction){
  if(!total)return fraction<.5?0:255;
  const target=total*fraction;let count=0;
  for(let i=0;i<256;i++){count+=hist[i];if(count>=target)return i}
  return 255;
}
function render(){
  if(!source)return;
  drawPhoto(source);
  const image=ctx.getImageData(0,0,W,H);
  const p=image.data;
  const black=new Uint8Array(FB);black.fill(255);
  const red=new Uint8Array(FB);
  const redMask=new Uint8Array(W*H);
  const whiteMask=new Uint8Array(W*H);
  const luma=new Float32Array(W*H);
  const channelR=new Float32Array(W*H);
  const channelG=new Float32Array(W*H);
  const channelB=new Float32Array(W*H);
  const tri=document.querySelector('input[name=mode]:checked').value==='tri';
  const algorithm=algorithmSelect.value;
  function paint(x,y,ink){
    const i=y*W+x,q=i*4,index=y*RB+(x>>3),mask=0x80>>(x&7);
    if(ink===1){black[index]&=~mask;p[q]=p[q+1]=p[q+2]=20}
    else if(ink===2){red[index]|=mask;p[q]=196;p[q+1]=43;p[q+2]=43}
    else{p[q]=p[q+1]=p[q+2]=255}
    p[q+3]=255;
  }
  if(algorithm==='waveshare'){
    const palette=tri?[[0,0,0],[255,255,255],[127,0,0]]:[[0,0,0],[255,255,255]];
    const errors=[new Array(W),new Array(W)];
    for(let x=0;x<W;x++)errors[1][x]=[0,0,0];
    let current=0,next=1;
    function addError(target,r,g,b,weight){
      target[0]+=r*weight/32;target[1]+=g*weight/32;target[2]+=b*weight/32;
    }
    for(let y=0;y<H;y++){
      current=((next=current)+1)&1;
      for(let x=0;x<W;x++)errors[next][x]=[0,0,0];
      for(let x=0;x<W;x++){
        const i=y*W+x,q=i*4,old=errors[current][x];
        let r=p[q]+old[0],g=p[q+1]+old[1],b=p[q+2]+old[2];
        let nearest=0,best=Infinity;
        for(let color=0;color<palette.length;color++){
          const dr=r-palette[color][0],dg=g-palette[color][1],db=b-palette[color][2];
          const distance=dr*dr+dg*dg+db*db;
          if(distance<best){best=distance;nearest=color}
        }
        const target=palette[nearest];
        paint(x,y,nearest===0?1:nearest===2?2:0);
        r-=target[0];g-=target[1];b-=target[2];
        if(x===0){
          addError(errors[next][x],r,g,b,7);
          addError(errors[next][x+1],r,g,b,2);
          addError(errors[current][x+1],r,g,b,7);
        }else if(x===W-1){
          addError(errors[next][x-1],r,g,b,7);
          addError(errors[next][x],r,g,b,9);
        }else{
          addError(errors[next][x-1],r,g,b,3);
          addError(errors[next][x],r,g,b,5);
          addError(errors[next][x+1],r,g,b,1);
          addError(errors[current][x+1],r,g,b,7);
        }
      }
    }
    ctx.putImageData(image,0,0);
    payload=new Uint8Array(FB*2);payload.set(black,0);payload.set(red,FB);
    upload.disabled=false;status.textContent='预览完成 · 微雪官方抖动';return;
  }
  const automatic=algorithm==='auto-photo';
  const bright=automatic?0:value('brightness');
  const contrast=automatic?0:value('contrast');
  const factor=(259*(contrast+255))/(255*(259-contrast));
  const redCut=automatic?74:108-calibratedValue('redLevel')*.72;
  const sharpness=automatic?.08:value('sharpness')/100;
  const denoise=automatic?.72:value('denoise')/100;
  const blackPoint=calibratedValue('blackPoint');
  const histogram=new Uint32Array(256);
  let histogramCount=0;
  for(let i=0;i<W*H;i++){
    const q=i*4,r=p[q],g=p[q+1],b=p[q+2];
    const protectedWhite=r>252&&g>252&&b>252;
    whiteMask[i]=protectedWhite;
    channelR[i]=r;channelG[i]=g;channelB[i]=b;
    const gray=.2126*r+.7152*g+.0722*b;
    luma[i]=gray;
    if(!protectedWhite){histogram[Math.round(gray)]++;histogramCount++}
  }
  let autoGain=1,autoOffset=0;
  if(automatic&&histogramCount){
    const low=percentile(histogram,histogramCount,.03);
    const high=percentile(histogram,histogramCount,.97);
    autoGain=clampValue(200/Math.max(1,high-low),.88,1.12);
    autoOffset=clampValue(132-(low+high)*.5*autoGain,-12,18);
  }
  for(let i=0;i<W*H;i++){
    if(whiteMask[i]){luma[i]=255;continue}
    if(automatic){
      luma[i]=clamp(luma[i]*autoGain+autoOffset);
    }else{
      const r=clamp(factor*(channelR[i]-128)+128+bright);
      const g=clamp(factor*(channelG[i]-128)+128+bright);
      const b=clamp(factor*(channelB[i]-128)+128+bright);
      channelR[i]=r;channelG[i]=g;channelB[i]=b;
      luma[i]=.2126*r+.7152*g+.0722*b;
    }
  }
  if(denoise>0){
    const raw=new Float32Array(luma),neighbors=[[-1,-1],[0,-1],[1,-1],[-1,0],[1,0],[-1,1],[0,1],[1,1]];
    const tolerance=22+denoise*55;
    for(let y=1;y<H-1;y++)for(let x=1;x<W-1;x++){
      const i=y*W+x;if(whiteMask[i])continue;
      const center=raw[i];let sum=center*2,weight=2;
      for(const offset of neighbors){
        const j=(y+offset[1])*W+x+offset[0];if(whiteMask[j])continue;
        const w=Math.max(0,1-Math.abs(raw[j]-center)/tolerance);
        sum+=raw[j]*w;weight+=w;
      }
      const detail=(sum/weight-center)*denoise;
      luma[i]=clamp(center+detail);
      channelR[i]=clamp(channelR[i]+detail);channelG[i]=clamp(channelG[i]+detail);channelB[i]=clamp(channelB[i]+detail);
    }
  }
  const redCandidate=new Uint8Array(W*H);
  for(let i=0;i<W*H;i++){
    const r=channelR[i],g=channelG[i],b=channelB[i],redness=r-(g+b)/2;
    if(automatic){
      const saturation=(r-Math.min(r,g,b))/Math.max(1,r);
      redCandidate[i]=tri&&r>125&&redness>redCut&&r-g>70&&r-b>60&&g<r*.55&&b<r*.60&&saturation>.60;
    }else{
      const redRatio=.54+calibratedValue('redLevel')*.0015;
      redCandidate[i]=tri&&r>105&&redness>redCut&&g<r*redRatio&&b<r*(redRatio+.04)&&r-g>42&&g+b<r*1.02;
    }
  }
  if(automatic){
    for(let y=1;y<H-1;y++)for(let x=1;x<W-1;x++){
      const i=y*W+x;if(!redCandidate[i])continue;
      let nearby=0;
      for(let oy=-1;oy<=1;oy++)for(let ox=-1;ox<=1;ox++)nearby+=redCandidate[(y+oy)*W+x+ox];
      redMask[i]=nearby>=4;
    }
  }else{
    redMask.set(redCandidate);
  }
  const base=new Float32Array(luma);
  for(let y=1;y<H-1;y++)for(let x=1;x<W-1;x++){
    const i=y*W+x;
    const blur=(base[i]*4+base[i-1]+base[i+1]+base[i-W]+base[i+W])/8;
    luma[i]=clamp(base[i]+sharpness*(base[i]-blur));
    const detail=luma[i]-base[i];
    channelR[i]=clamp(channelR[i]+detail);
    channelG[i]=clamp(channelG[i]+detail);
    channelB[i]=clamp(channelB[i]+detail);
  }
  let cleanBlack=null;
  if(automatic){
    const processedHistogram=new Uint32Array(256);
    let processedCount=0,processedSum=0;
    for(let i=0;i<W*H;i++)if(!redMask[i]&&!whiteMask[i]){
      const level=Math.round(clamp(luma[i]));
      processedHistogram[level]++;processedCount++;processedSum+=level;
    }
    let backgroundWeight=0,backgroundSum=0,bestVariance=-1,otsu=128;
    for(let level=0;level<256;level++){
      backgroundWeight+=processedHistogram[level];
      if(!backgroundWeight)continue;
      const foregroundWeight=processedCount-backgroundWeight;
      if(!foregroundWeight)break;
      backgroundSum+=level*processedHistogram[level];
      const backgroundMean=backgroundSum/backgroundWeight;
      const foregroundMean=(processedSum-backgroundSum)/foregroundWeight;
      const variance=backgroundWeight*foregroundWeight*(backgroundMean-foregroundMean)**2;
      if(variance>bestVariance){bestVariance=variance;otsu=level}
    }
    const globalPoint=clampValue(otsu,108,154);
    const stride=W+1,integral=new Float32Array((W+1)*(H+1));
    for(let y=0;y<H;y++){
      let rowSum=0;
      for(let x=0;x<W;x++){
        rowSum+=luma[y*W+x];
        integral[(y+1)*stride+x+1]=integral[y*stride+x+1]+rowSum;
      }
    }
    cleanBlack=new Uint8Array(W*H);
    const radius=6;
    for(let y=0;y<H;y++)for(let x=0;x<W;x++){
      const i=y*W+x;if(redMask[i]||whiteMask[i])continue;
      const x0=Math.max(0,x-radius),x1=Math.min(W-1,x+radius);
      const y0=Math.max(0,y-radius),y1=Math.min(H-1,y+radius);
      const area=(x1-x0+1)*(y1-y0+1);
      const local=(integral[(y1+1)*stride+x1+1]-integral[y0*stride+x1+1]-integral[(y1+1)*stride+x0]+integral[y0*stride+x0])/area;
      const threshold=globalPoint*.78+local*.22-4;
      cleanBlack[i]=luma[i]<threshold;
    }
    for(let pass=0;pass<2;pass++){
      const sourceMask=cleanBlack,next=new Uint8Array(sourceMask);
      for(let y=1;y<H-1;y++)for(let x=1;x<W-1;x++){
        const i=y*W+x;if(redMask[i]||whiteMask[i]){next[i]=0;continue}
        let neighbors=0;
        for(let oy=-1;oy<=1;oy++)for(let ox=-1;ox<=1;ox++)neighbors+=sourceMask[(y+oy)*W+x+ox];
        next[i]=neighbors>=5||luma[i]<globalPoint-26;
      }
      cleanBlack=next;
    }
  }
  const FS=[[1,0,7/16],[-1,1,3/16],[0,1,5/16],[1,1,1/16]];
  function diffuseLuma(x,y,error,dir){
    for(const step of FS){
      const nx=x+step[0]*dir,ny=y+step[1];
      if(nx>=0&&nx<W&&ny<H){
        const i=ny*W+nx;
        if(!redMask[i]&&!whiteMask[i])luma[i]+=error*step[2];
      }
    }
  }
  function diffuseColor(x,y,errors,dir){
    for(const step of FS){
      const nx=x+step[0]*dir,ny=y+step[1];
      if(nx>=0&&nx<W&&ny<H){
        const i=ny*W+nx;
        if(!whiteMask[i]){
          channelR[i]+=errors[0]*step[2];channelG[i]+=errors[1]*step[2];channelB[i]+=errors[2]*step[2];
        }
      }
    }
  }
  if(algorithm==='tri-floyd'){
    const blackBias=145/blackPoint;
    const redBias=1.45-calibratedValue('redLevel')*.0095;
    for(let y=0;y<H;y++){
      const dir=y&1?-1:1;
      for(let n=0;n<W;n++){
        const x=dir===1?n:W-1-n,i=y*W+x;
        if(whiteMask[i]){paint(x,y,0);continue}
        const r=clamp(channelR[i]),g=clamp(channelG[i]),b=clamp(channelB[i]);
        const whiteDistance=.3*(r-255)**2+.59*(g-255)**2+.11*(b-255)**2;
        const blackDistance=(.3*(r-20)**2+.59*(g-20)**2+.11*(b-20)**2)*blackBias;
        const redDistance=tri&&redMask[i]?(.3*(r-196)**2+.59*(g-43)**2+.11*(b-43)**2)*redBias:Infinity;
        const ink=redDistance<whiteDistance&&redDistance<blackDistance?2:blackDistance<whiteDistance?1:0;
        const target=ink===2?[196,43,43]:ink===1?[20,20,20]:[255,255,255];
        paint(x,y,ink);diffuseColor(x,y,[r-target[0],g-target[1],b-target[2]],dir);
      }
    }
  }else for(let y=0;y<H;y++){
    const dir=automatic&&(y&1)?-1:1;
    for(let n=0;n<W;n++){
      const x=dir===1?n:W-1-n,i=y*W+x;
      const isRed=redMask[i]===1,isWhite=whiteMask[i]===1,gray=clamp(luma[i]);
      let isBlack=automatic?cleanBlack[i]===1:gray<blackPoint;
      isBlack=!isRed&&!isWhite&&isBlack;
      paint(x,y,isBlack?1:isRed?2:0);
      if(!automatic&&!isRed&&!isWhite)diffuseLuma(x,y,gray-(isBlack?0:255),dir);
    }
  }
  ctx.putImageData(image,0,0);
  payload=new Uint8Array(FB*2);payload.set(black,0);payload.set(red,FB);
  upload.disabled=false;
  status.textContent=algorithm==='tri-floyd'?'预览完成 · 三色 FS':automatic?'预览完成 · 黑白块面':'预览完成 · 最初 FS';
}
function updateCompareSelection(){compareButtons.forEach(button=>button.setAttribute('aria-pressed',button.dataset.algorithm===algorithmSelect.value))}
function renderComparisons(){
  if(!source)return;
  const selected=algorithmSelect.value,savedStatus=status.textContent;
  let selectedImage=null,selectedPayload=null,selectedStatus='';
  compareStatus.textContent='正在生成';
  for(const button of compareButtons){
    algorithmSelect.value=button.dataset.algorithm;render();
    button.querySelector('canvas').getContext('2d').putImageData(ctx.getImageData(0,0,W,H),0,0);
    if(button.dataset.algorithm===selected){
      selectedImage=ctx.getImageData(0,0,W,H);selectedPayload=payload;selectedStatus=status.textContent;
    }
  }
  algorithmSelect.value=selected;
  if(selectedImage)ctx.putImageData(selectedImage,0,0);
  payload=selectedPayload;status.textContent=savedStatus.startsWith('照片已载入')?savedStatus:selectedStatus;
  updateDetailControls();updateCompareSelection();compareStatus.textContent='点击小图切换';
}
function scheduleComparison(delay=260){clearTimeout(compareTimer);if(source)compareTimer=setTimeout(renderComparisons,delay)}
function scheduleRender(){
  clearTimeout(compareTimer);
  if(renderPending)return;
  renderPending=true;requestAnimationFrame(()=>{renderPending=false;render();scheduleComparison()});
}
const shortcutMode=new URLSearchParams(location.search).get('shortcut')==='1';
function activateSource(image,isCalibration=false){
  source=image;calibrationMode=isCalibration;
  if(!isCalibration)photoSource=image;
  rotation=0;resetEdit();rotateButton.disabled=isCalibration;
  zoomButtons.forEach(button=>button.disabled=isCalibration);render();scheduleComparison(120);
  calibrationTarget.textContent=isCalibration&&photoSource?'返回照片':'显示校准图';
}
function loadImageBlob(blob){
  const image=new Image();
  image.onload=()=>{
    activateSource(image,false);
    if(shortcutMode){
      upload.disabled=false;
      status.textContent='照片已载入 · 可编辑，确认后再上传';
    }
    URL.revokeObjectURL(image.src);
  };
  image.src=URL.createObjectURL(blob);
}
function createCalibrationTarget(){
  const target=document.createElement('canvas'),c=target.getContext('2d');target.width=W;target.height=H;
  c.fillStyle='#fff';c.fillRect(0,0,W,H);
  ['#000','#fff','#c42b2b'].forEach((color,index)=>{c.fillStyle=color;c.fillRect(index*50,0,index===2?52:50,34)});
  c.strokeStyle='#111';c.strokeRect(.5,.5,W-1,H-1);
  for(let step=0;step<12;step++){
    const level=Math.round(step*255/11);c.fillStyle=`rgb(${level},${level},${level})`;c.fillRect(4+step*12,42,12,54);
  }
  c.fillStyle='#fff';c.fillRect(4,104,144,42);c.fillStyle='#111';
  for(let x=6;x<148;x+=6)c.fillRect(x,104,x%18===0?2:1,42);
  for(let y=154;y<206;y+=4)for(let x=4;x<148;x+=4)if(((x+y)/4)&1)c.fillRect(x,y,4,4);
  const reds=['#f4c7c7','#e88b8b','#dc5555','#c92d2d','#a71919','#740d0d'];
  reds.forEach((color,index)=>{c.fillStyle=color;c.fillRect(4+index*24,216,24,48)});
  c.fillStyle='#000';for(let width=1;width<=4;width++)c.fillRect(8+(width-1)*34,274,width,15);
  const image=new Image();image.onload=()=>{
    algorithmSelect.value='tri-floyd';document.querySelector('input[name=mode][value=tri]').checked=true;
    activateSource(image,true);updateDetailControls();updateCompareSelection();status.textContent='校准图已载入';
  };
  image.src=target.toDataURL('image/png');
}
file.addEventListener('change',()=>{
  const selected=file.files[0];if(!selected)return;
  loadImageBlob(selected);
});
if(shortcutMode){
  status.textContent='正在读取快捷指令照片';
  fetch('/api/shortcut/photo',{cache:'no-store'}).then(response=>{
    if(!response.ok)throw new Error('没有收到快捷指令照片');
    return response.blob();
  }).then(loadImageBlob).catch(error=>{status.textContent=error.message});
}
rotateButton.addEventListener('click',()=>{rotation=(rotation+90)%360;resetEdit();scheduleRender()});
document.querySelectorAll('input[name=mode]').forEach(el=>el.addEventListener('change',scheduleRender));
document.querySelectorAll('input[name=fit]').forEach(el=>el.addEventListener('change',()=>{resetEdit();scheduleRender()}));
document.querySelector('#ratio').addEventListener('change',()=>{resetEdit();scheduleRender()});
algorithmSelect.addEventListener('change',()=>{updateDetailControls();updateCompareSelection();scheduleRender()});
compareButtons.forEach(button=>button.addEventListener('click',()=>{algorithmSelect.value=button.dataset.algorithm;updateDetailControls();updateCompareSelection();scheduleRender()}));
calibrationTarget.addEventListener('click',()=>{
  if(calibrationMode&&photoSource){activateSource(photoSource,false);status.textContent='已返回照片';return}
  createCalibrationTarget();
});
['blackOffset','redOffset'].forEach(id=>document.querySelector('#'+id).addEventListener('input',saveCalibrationProfile));
document.querySelector('#resetCalibration').addEventListener('click',()=>{
  document.querySelector('#blackOffset').value=0;document.querySelector('#redOffset').value=0;saveCalibrationProfile();
});
updateDetailControls();
updateCompareSelection();
previewScale.addEventListener('input',applyPreviewScale);
controls.forEach(id=>{
  const el=document.querySelector('#'+id),out=document.querySelector('#'+id+'Value');
  el.addEventListener('input',()=>{out.value=el.value;scheduleRender()});
});
document.querySelector('#zoomOut').addEventListener('click',()=>setZoom(edit.zoom/1.25));
document.querySelector('#zoomIn').addEventListener('click',()=>setZoom(edit.zoom*1.25));
document.querySelector('#resetCrop').addEventListener('click',()=>{resetEdit();scheduleRender()});
function canvasPoint(point){
  const rect=canvas.getBoundingClientRect();
  return{x:(point.clientX-rect.left)*W/rect.width,y:(point.clientY-rect.top)*H/rect.height};
}
function beginGesture(){
  const points=[...pointers.values()].map(canvasPoint);
  if(points.length===1)gesture={kind:'pan',point:points[0],x:edit.x,y:edit.y};
  else if(points.length>=2){
    const center={x:(points[0].x+points[1].x)/2,y:(points[0].y+points[1].y)/2};
    gesture={kind:'pinch',distance:Math.hypot(points[1].x-points[0].x,points[1].y-points[0].y),center,zoom:edit.zoom,x:edit.x,y:edit.y};
  }else gesture=null;
}
canvas.addEventListener('pointerdown',event=>{
  if(!source)return;event.preventDefault();canvas.setPointerCapture(event.pointerId);
  pointers.set(event.pointerId,event);canvas.classList.add('dragging');beginGesture();
});
canvas.addEventListener('pointermove',event=>{
  if(!pointers.has(event.pointerId)||!gesture)return;event.preventDefault();pointers.set(event.pointerId,event);
  const points=[...pointers.values()].map(canvasPoint);
  if(points.length===1&&gesture.kind==='pan'){
    edit.x=gesture.x+points[0].x-gesture.point.x;edit.y=gesture.y+points[0].y-gesture.point.y;
  }else if(points.length>=2&&gesture.kind==='pinch'){
    const center={x:(points[0].x+points[1].x)/2,y:(points[0].y+points[1].y)/2};
    const distance=Math.hypot(points[1].x-points[0].x,points[1].y-points[0].y);
    const min=document.querySelector('input[name=fit]:checked').value==='crop'?1:.35;
    edit.zoom=clampValue(gesture.zoom*distance/Math.max(1,gesture.distance),min,6);
    const ratio=edit.zoom/gesture.zoom;
    edit.x=center.x-W/2-(gesture.center.x-W/2-gesture.x)*ratio;
    edit.y=center.y-H/2-(gesture.center.y-H/2-gesture.y)*ratio;
    updateZoom();
  }else{beginGesture();return}
  clampEdit();scheduleRender();
});
function endPointer(event){
  pointers.delete(event.pointerId);beginGesture();
  if(!pointers.size)canvas.classList.remove('dragging');
}
canvas.addEventListener('pointerup',endPointer);
canvas.addEventListener('pointercancel',endPointer);
canvas.addEventListener('wheel',event=>{
  if(!source)return;event.preventDefault();
  const point=canvasPoint(event),anchorX=point.x-W/2,anchorY=point.y-H/2;
  if(event.ctrlKey||event.metaKey)setZoom(edit.zoom*Math.exp(-event.deltaY*.012),anchorX,anchorY);
  else{
    const rect=canvas.getBoundingClientRect();edit.x-=event.deltaX*W/rect.width;edit.y-=event.deltaY*H/rect.height;
    clampEdit();scheduleRender();
  }
},{passive:false});
canvas.addEventListener('gesturestart',event=>{if(!source)return;event.preventDefault();safariGestureZoom=edit.zoom});
canvas.addEventListener('gesturechange',event=>{if(!source)return;event.preventDefault();setZoom(safariGestureZoom*event.scale)});
async function waitForDisplay(){
  for(;;){
    await new Promise(resolve=>setTimeout(resolve,1200));
    try{
      const response=await fetch('/status',{cache:'no-store'});
      const data=await response.json();
      if(data.state==='done'){
        status.textContent='完成，用时 '+(data.ms/1000).toFixed(1)+' 秒';
        upload.disabled=!payload;rotateButton.disabled=!source||calibrationMode;clearButton.disabled=false;return
      }
      if(data.state==='error'){throw new Error('屏幕刷新失败')}
      status.textContent='正在刷新屏幕';
    }catch(error){
      if(error.message==='屏幕刷新失败'){
        status.textContent=error.message;upload.disabled=!payload;rotateButton.disabled=!source||calibrationMode;clearButton.disabled=false;return
      }
    }
  }
}
upload.addEventListener('click',async()=>{
  if(!payload)return;
  upload.disabled=true;rotateButton.disabled=true;clearButton.disabled=true;status.textContent='正在上传';
  const form=new FormData();form.append('frame',new Blob([payload],{type:'application/octet-stream'}),'frame.bin');
  try{
    const response=await fetch('/upload',{method:'POST',body:form});
    const data=await response.json();
    if(!response.ok||!data.ok)throw new Error(data.error||'上传失败');
    status.textContent='正在刷新屏幕';waitForDisplay();
  }catch(error){status.textContent=error.message;upload.disabled=false;rotateButton.disabled=calibrationMode;clearButton.disabled=false}
});
clearButton.addEventListener('click',async()=>{
  upload.disabled=true;rotateButton.disabled=true;clearButton.disabled=true;status.textContent='正在清屏';
  try{
    const response=await fetch('/clear',{method:'POST'});
    const data=await response.json();
    if(!response.ok||!data.ok)throw new Error(data.error||'清屏失败');
    waitForDisplay();
  }catch(error){status.textContent=error.message;upload.disabled=!payload;rotateButton.disabled=!source||calibrationMode;clearButton.disabled=false}
});
document.querySelector('#ota').addEventListener('click',()=>{window.location.assign('/update')});
fetch('/api/status',{cache:'no-store'}).then(response=>response.json()).then(data=>{
  const host=data.mdns?data.hostname:data.ip;deviceAddress.textContent=host;deviceAddress.href='http://'+host+'/';
}).catch(()=>{});
</script>
</body>
</html>
)HTML";

const uint8_t *findGlyph(char code) {
  for (const auto &glyph : FONT) {
    if (glyph.code == code) {
      return glyph.columns;
    }
  }
  return FONT[0].columns;
}

void setNativePixel(uint16_t x, uint16_t y, Ink ink) {
  if (x >= NATIVE_WIDTH || y >= NATIVE_HEIGHT) {
    return;
  }
  const size_t index = static_cast<size_t>(y) * ROW_BYTES + x / 8;
  const uint8_t mask = 0x80U >> (x & 7U);
  if (ink == Ink::Black) {
    blackFrame[index] &= static_cast<uint8_t>(~mask);
    redFrame[index] &= static_cast<uint8_t>(~mask);
  } else if (ink == Ink::Red) {
    blackFrame[index] |= mask;
    redFrame[index] |= mask;
  } else {
    blackFrame[index] |= mask;
    redFrame[index] &= static_cast<uint8_t>(~mask);
  }
}

void setPixel(uint16_t x, uint16_t y, Ink ink) {
  if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) {
    return;
  }
  setNativePixel(x, y, ink);
}

void fillRect(int16_t x, int16_t y, int16_t width, int16_t height, Ink ink) {
  for (int16_t yy = 0; yy < height; ++yy) {
    for (int16_t xx = 0; xx < width; ++xx) {
      if (x + xx >= 0 && y + yy >= 0) {
        setPixel(static_cast<uint16_t>(x + xx),
                 static_cast<uint16_t>(y + yy), ink);
      }
    }
  }
}

void drawChar(int16_t x, int16_t y, char code, uint8_t scale, Ink ink) {
  const uint8_t *columns = findGlyph(code);
  for (uint8_t column = 0; column < 5; ++column) {
    for (uint8_t row = 0; row < 7; ++row) {
      if (columns[column] & (1U << row)) {
        fillRect(x + column * scale, y + row * scale, scale, scale, ink);
      }
    }
  }
}

int16_t textWidth(const char *value, uint8_t scale) {
  return static_cast<int16_t>(strlen(value) * 6U * scale - scale);
}

void drawTextCentered(int16_t y, const char *value, uint8_t scale, Ink ink) {
  int16_t x = (DISPLAY_WIDTH - textWidth(value, scale)) / 2;
  while (*value) {
    drawChar(x, y, *value++, scale, ink);
    x += 6 * scale;
  }
}

IPAddress currentIp() {
  return accessPointMode ? WiFi.softAPIP() : WiFi.localIP();
}

void drawIpFrame() {
  memset(blackFrame, 0xFF, sizeof(blackFrame));
  memset(redFrame, 0x00, sizeof(redFrame));
  fillRect(3, 3, 146, 4, Ink::Red);
  fillRect(3, 289, 146, 4, Ink::Black);
  fillRect(3, 3, 3, 290, Ink::Black);
  fillRect(146, 3, 3, 290, Ink::Black);
  drawTextCentered(42, "PHOTO EPD", 2, Ink::Black);
  drawTextCentered(105, "IP ADDRESS", 2, Ink::Red);
  const String address = currentIp().toString();
  const uint8_t scale = textWidth(address.c_str(), 2) <= 140 ? 2 : 1;
  drawTextCentered(157, address.c_str(), scale, Ink::Black);
}

void clearFrame() {
  memset(blackFrame, 0xFF, sizeof(blackFrame));
  memset(redFrame, 0x00, sizeof(redFrame));
}

void writeCommand(uint8_t command) {
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer(command);
  digitalWrite(PIN_CS, HIGH);
}

void writeData(uint8_t data) {
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer(data);
  digitalWrite(PIN_CS, HIGH);
}

bool waitReady(const char *stage, uint32_t *elapsedMs = nullptr) {
  const uint32_t started = millis();
  while (digitalRead(PIN_BUSY) == LOW) {
    if (millis() - started >= BUSY_TIMEOUT_MS) {
      Serial.print("ERROR busy timeout: ");
      Serial.println(stage);
      return false;
    }
    delay(1);
    yield();
  }
  const uint32_t elapsed = millis() - started;
  if (elapsedMs != nullptr) {
    *elapsedMs = elapsed;
  }
  Serial.print("READY ");
  Serial.print(stage);
  Serial.print(" in ");
  Serial.print(elapsed);
  Serial.println(" ms");
  return true;
}

void hardwareReset() {
  digitalWrite(PIN_RESET, LOW);
  delay(20);
  digitalWrite(PIN_RESET, HIGH);
  delay(20);
}

bool initializePanel() {
  hardwareReset();
  writeCommand(0x04);
  if (!waitReady("power-on")) {
    return false;
  }

  writeCommand(0x00);
  writeData(0x0F);
  writeData(0x89);

  writeCommand(0x01);
  writeData(0x03);
  writeData(0x00);
  writeData(0x21);
  writeData(0x21);
  writeData(0x01);

  writeCommand(0x41);
  writeData(0x07);

  writeCommand(0x61);
  writeData(0x98);
  writeData(0x01);
  writeData(0x28);

  writeCommand(0x50);
  writeData(0x97);

  writeCommand(0x2A);
  writeData(0x00);
  writeData(0x00);
  writeData(0x00);
  writeData(0xFF);
  writeData(0x00);
  return true;
}

void sendFrame(uint8_t command, const uint8_t *frame) {
  writeCommand(command);
  for (size_t i = 0; i < FRAME_BYTES; ++i) {
    writeData(frame[i]);
    if ((i & 0xFFU) == 0) {
      yield();
    }
  }
}

bool powerOffPanel() {
  writeCommand(0x02);
  if (!waitReady("power-off")) {
    return false;
  }
  delay(100);
  writeCommand(0x07);
  writeData(0xA5);
  return true;
}

bool refreshDisplay(uint32_t *elapsedMs) {
  Serial.println("EPD display refresh start");
  if (!initializePanel()) {
    return false;
  }
  sendFrame(0x10, blackFrame);
  sendFrame(0x13, redFrame);
  writeCommand(0x12);
  delay(20);
  const bool refreshed = waitReady("display refresh", elapsedMs);
  const bool poweredOff = powerOffPanel();
  return refreshed && poweredOff;
}

bool writeScreenMarker() {
  if (!fileSystemReady) {
    return false;
  }
  File marker = LittleFS.open(SCREEN_MARKER_PATH, "w");
  if (!marker) {
    return false;
  }
  marker.print("1");
  marker.close();
  return true;
}

bool savePhoto() {
  if (!fileSystemReady) {
    return false;
  }
  LittleFS.remove(PHOTO_TEMP_PATH);
  File file = LittleFS.open(PHOTO_TEMP_PATH, "w");
  if (!file) {
    return false;
  }
  const bool written = file.write(blackFrame, FRAME_BYTES) == FRAME_BYTES &&
                       file.write(redFrame, FRAME_BYTES) == FRAME_BYTES;
  file.close();
  if (!written) {
    LittleFS.remove(PHOTO_TEMP_PATH);
    return false;
  }
  LittleFS.remove(PHOTO_PATH);
  return LittleFS.rename(PHOTO_TEMP_PATH, PHOTO_PATH);
}

bool loadPhoto() {
  if (!fileSystemReady || !LittleFS.exists(PHOTO_PATH)) {
    return false;
  }
  File file = LittleFS.open(PHOTO_PATH, "r");
  if (!file || file.size() != UPLOAD_BYTES) {
    file.close();
    return false;
  }
  const bool read = file.read(blackFrame, FRAME_BYTES) == FRAME_BYTES &&
                    file.read(redFrame, FRAME_BYTES) == FRAME_BYTES;
  file.close();
  return read;
}

const char *stateName() {
  switch (displayState) {
    case DisplayState::Queued:
      return "queued";
    case DisplayState::Refreshing:
      return "refreshing";
    case DisplayState::Done:
      return "done";
    case DisplayState::Error:
      return "error";
    default:
      return "idle";
  }
}

bool displayIsBusy() {
  return refreshQueued || storedPhotoQueued || clearScreenQueued ||
         ipScreenQueued || displayState == DisplayState::Refreshing;
}

String statusJson() {
  String json = F("{\"state\":\"");
  json += stateName();
  json += F("\",\"ms\":");
  json += lastRefreshMs;
  json += F(",\"ip\":\"");
  json += currentIp().toString();
  json += F("\",\"hostname\":\"epd-photo.local\",\"mdns\":");
  json += mdnsReady ? F("true") : F("false");
  json += F(",\"stored\":");
  json += fileSystemReady && LittleFS.exists(PHOTO_PATH) ? F("true") : F("false");
  json += F(",\"version\":\"");
  json += FIRMWARE_VERSION;
  json += F("\",\"uptime_ms\":");
  json += millis();
  json += F(",\"free_heap\":");
  json += ESP.getFreeHeap();
  json += F(",\"mode\":\"");
  json += accessPointMode ? F("ap") : F("station");
  json += F("\",\"rssi\":");
  json += accessPointMode ? 0 : WiFi.RSSI();
  json += '}';
  return json;
}

void sendJson(int code, const String &json) {
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(code, F("application/json"), json);
}

void sendBusyError() {
  sendJson(409, F("{\"ok\":false,\"error\":\"屏幕正在刷新\"}"));
}

void handleRoot() {
  server.sendHeader(F("Cache-Control"), F("no-store, no-cache, must-revalidate, max-age=0"));
  server.sendHeader(F("Pragma"), F("no-cache"));
  server.sendHeader(F("Expires"), F("0"));
  server.send_P(200, PSTR("text/html; charset=utf-8"), PAGE);
}

void handleStatus() {
  sendJson(200, statusJson());
}

void handleShortcutPage() {
  server.sendHeader(F("Cache-Control"), F("no-store, no-cache, must-revalidate, max-age=0"));
  server.sendHeader(F("Pragma"), F("no-cache"));
  server.sendHeader(F("Expires"), F("0"));
  server.send_P(200, PSTR("text/html; charset=utf-8"), PAGE);
}

void handleShortcutPhotoGet() {
  if (!fileSystemReady || !LittleFS.exists(SHORTCUT_SOURCE_PATH)) {
    server.send(404, F("text/plain; charset=utf-8"), F("没有快捷指令照片"));
    return;
  }
  File photo = LittleFS.open(SHORTCUT_SOURCE_PATH, "r");
  if (!photo) {
    server.send(500, F("text/plain; charset=utf-8"), F("读取快捷指令照片失败"));
    return;
  }
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.streamFile(photo, F("image/jpeg"));
  photo.close();
}

void handleShortcutSourceChunk() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    shortcutSourceBytes = 0;
    shortcutSourceUploadValid = fileSystemReady && !displayIsBusy();
    LittleFS.remove(SHORTCUT_SOURCE_TEMP_PATH);
    if (shortcutSourceUploadValid) {
      shortcutSourceUpload = LittleFS.open(SHORTCUT_SOURCE_TEMP_PATH, "w");
      shortcutSourceUploadValid = static_cast<bool>(shortcutSourceUpload);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!shortcutSourceUploadValid ||
        shortcutSourceBytes + upload.currentSize > SHORTCUT_SOURCE_MAX_BYTES ||
        shortcutSourceUpload.write(upload.buf, upload.currentSize) != upload.currentSize) {
      shortcutSourceUploadValid = false;
      if (shortcutSourceUpload) shortcutSourceUpload.close();
      return;
    }
    shortcutSourceBytes += upload.currentSize;
    yield();
  } else if (upload.status == UPLOAD_FILE_END) {
    if (shortcutSourceUpload) shortcutSourceUpload.close();
    shortcutSourceUploadValid = shortcutSourceUploadValid && shortcutSourceBytes > 0;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (shortcutSourceUpload) shortcutSourceUpload.close();
    shortcutSourceUploadValid = false;
    LittleFS.remove(SHORTCUT_SOURCE_TEMP_PATH);
  }
}

void handleShortcutSourceComplete() {
  if (!shortcutSourceUploadValid || shortcutSourceBytes == 0) {
    LittleFS.remove(SHORTCUT_SOURCE_TEMP_PATH);
    sendJson(400, F("{\"ok\":false,\"error\":\"快捷指令照片数据无效或过大\"}"));
    return;
  }
  LittleFS.remove(SHORTCUT_SOURCE_PATH);
  if (!LittleFS.rename(SHORTCUT_SOURCE_TEMP_PATH, SHORTCUT_SOURCE_PATH)) {
    sendJson(500, F("{\"ok\":false,\"error\":\"保存快捷指令照片失败\"}"));
    return;
  }
  sendJson(201, F("{\"ok\":true,\"action\":\"open_shortcut_page\"}"));
}

void handleUploadChunk() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    uploadOffset = 0;
    uploadValid = !refreshQueued && !storedPhotoQueued && !clearScreenQueued &&
                  !ipScreenQueued && displayState != DisplayState::Refreshing;
    Serial.println("Photo upload start");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!uploadValid || uploadOffset + upload.currentSize > UPLOAD_BYTES) {
      uploadValid = false;
      return;
    }
    for (size_t i = 0; i < upload.currentSize; ++i) {
      const size_t target = uploadOffset + i;
      if (target < FRAME_BYTES) {
        blackFrame[target] = upload.buf[i];
      } else {
        redFrame[target - FRAME_BYTES] = upload.buf[i];
      }
    }
    uploadOffset += upload.currentSize;
    yield();
  } else if (upload.status == UPLOAD_FILE_END) {
    uploadValid = uploadValid && uploadOffset == UPLOAD_BYTES;
    Serial.print("Photo upload bytes: ");
    Serial.println(uploadOffset);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    uploadValid = false;
    Serial.println("Photo upload aborted");
  }
}

void handleUploadComplete() {
  if (!uploadValid || uploadOffset != UPLOAD_BYTES) {
    server.send(400, F("application/json"),
                F("{\"ok\":false,\"error\":\"图像数据大小错误\"}"));
    return;
  }
  if (!savePhoto()) {
    server.send(500, F("application/json"),
                F("{\"ok\":false,\"error\":\"保存到闪存失败\"}"));
    return;
  }
  storedPhotoQueued = false;
  ipScreenQueued = false;
  refreshQueued = true;
  displayState = DisplayState::Queued;
  server.send(202, F("application/json"), F("{\"ok\":true}"));
}

void handleClear() {
  if (displayIsBusy()) {
    sendBusyError();
    return;
  }
  clearScreenQueued = true;
  displayState = DisplayState::Queued;
  sendJson(202, F("{\"ok\":true,\"action\":\"clear\"}"));
}

void handleReload() {
  if (displayIsBusy()) {
    sendBusyError();
    return;
  }
  if (!fileSystemReady || !LittleFS.exists(PHOTO_PATH)) {
    sendJson(404, F("{\"ok\":false,\"error\":\"没有已保存的照片\"}"));
    return;
  }
  storedPhotoQueued = true;
  displayState = DisplayState::Queued;
  sendJson(202, F("{\"ok\":true,\"action\":\"reload\"}"));
}

void handleIpScreen() {
  if (displayIsBusy()) {
    sendBusyError();
    return;
  }
  ipScreenQueued = true;
  displayState = DisplayState::Queued;
  sendJson(202, F("{\"ok\":true,\"action\":\"ip\"}"));
}

void startNetwork() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    accessPointMode = false;
    Serial.print("Wi-Fi IP: ");
    Serial.println(WiFi.localIP());
    mdnsReady = MDNS.begin(MDNS_HOSTNAME);
    if (mdnsReady) {
      MDNS.addService("http", "tcp", 80);
      MDNS.addService("epd-photo", "tcp", TCP_CONTROL_PORT);
      Serial.println("mDNS: http://epd-photo.local/");
    } else {
      Serial.println("mDNS start failed; use the IP address");
    }
    return;
  }

  Serial.println("Wi-Fi connection failed; starting fallback AP");
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
  accessPointMode = true;
  Serial.print("Fallback AP SSID: ");
  Serial.println(AP_SSID);
  Serial.print("Fallback AP IP: ");
  Serial.println(WiFi.softAPIP());
}

void startWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/shortcut", HTTP_GET, handleShortcutPage);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/shortcut/photo", HTTP_GET, handleShortcutPhotoGet);
  server.on("/api/shortcut/photo", HTTP_POST, handleShortcutSourceComplete,
            handleShortcutSourceChunk);
  server.on("/upload", HTTP_POST, handleUploadComplete, handleUploadChunk);
  server.on("/api/frame", HTTP_POST, handleUploadComplete, handleUploadChunk);
  server.on("/clear", HTTP_POST, handleClear);
  server.on("/api/display/clear", HTTP_POST, handleClear);
  server.on("/api/display/reload", HTTP_POST, handleReload);
  server.on("/api/display/ip", HTTP_POST, handleIpScreen);
  httpUpdater.setup(&server, F("/update"), String(OTA_USERNAME),
                    String(OTA_PASSWORD));
  server.onNotFound(handleRoot);
  server.begin();
  Serial.println("HTTP server and OTA updater started");
}

void sendTcp(const String &response) {
  if (tcpClient && tcpClient.connected()) {
    tcpClient.print(response);
    tcpClient.print('\n');
  }
}

void handleTcpCommand(String command) {
  command.trim();
  command.toUpperCase();
  if (command == "STATUS") {
    sendTcp(statusJson());
  } else if (command == "RELOAD") {
    if (displayIsBusy()) {
      sendTcp("ERR BUSY");
    } else if (!fileSystemReady || !LittleFS.exists(PHOTO_PATH)) {
      sendTcp("ERR NO_STORED_PHOTO");
    } else {
      storedPhotoQueued = true;
      displayState = DisplayState::Queued;
      sendTcp("OK QUEUED RELOAD");
    }
  } else if (command == "CLEAR") {
    if (displayIsBusy()) {
      sendTcp("ERR BUSY");
    } else {
      clearScreenQueued = true;
      displayState = DisplayState::Queued;
      sendTcp("OK QUEUED CLEAR");
    }
  } else if (command == "IP") {
    if (displayIsBusy()) {
      sendTcp("ERR BUSY");
    } else {
      ipScreenQueued = true;
      displayState = DisplayState::Queued;
      sendTcp("OK QUEUED IP");
    }
  } else if (command == "HELP") {
    sendTcp("OK COMMANDS STATUS RELOAD CLEAR IP HELP");
  } else {
    sendTcp("ERR UNKNOWN_COMMAND");
  }
}

void pollTcpServer() {
  if (!tcpClient || !tcpClient.connected()) {
    WiFiClient candidate = tcpServer.available();
    if (candidate) {
      if (tcpClient) {
        tcpClient.stop();
      }
      tcpClient = candidate;
      tcpCommand = "";
      tcpClient.setNoDelay(true);
      sendTcp("EPD PHOTO TCP READY; type HELP");
    }
  }
  if (!tcpClient || !tcpClient.connected()) {
    return;
  }
  while (tcpClient.available()) {
    const char value = static_cast<char>(tcpClient.read());
    if (value == '\n') {
      handleTcpCommand(tcpCommand);
      tcpCommand = "";
    } else if (value != '\r') {
      if (tcpCommand.length() >= TCP_COMMAND_MAX_BYTES) {
        tcpCommand = "";
        sendTcp("ERR COMMAND_TOO_LONG");
      } else {
        tcpCommand += value;
      }
    }
  }
}

void pollButton() {
  const bool raw = digitalRead(PIN_BUTTON);
  if (raw != buttonRawState) {
    buttonRawState = raw;
    buttonChangedAt = millis();
  }
  if (millis() - buttonChangedAt >= 40 && raw != buttonStableState) {
    buttonStableState = raw;
    if (buttonStableState == LOW) {
      buttonWasPressed = true;
      buttonIpIndicated = false;
      buttonClearIndicated = false;
      buttonPressedAt = millis();
    } else if (buttonWasPressed) {
      const uint32_t heldMs = millis() - buttonPressedAt;
      buttonWasPressed = false;
      buttonIpIndicated = false;
      buttonClearIndicated = false;
      digitalWrite(LED_BUILTIN, HIGH);
      const bool busy = refreshQueued || storedPhotoQueued || clearScreenQueued ||
                        ipScreenQueued ||
                        displayState == DisplayState::Refreshing;
      if (busy) {
        Serial.println("FLASH button ignored while display is busy");
      } else if (heldMs >= BUTTON_CLEAR_PRESS_MS) {
        clearScreenQueued = true;
        Serial.println("FLASH extra-long press; clear screen queued");
      } else if (heldMs >= BUTTON_IP_PRESS_MS) {
        ipScreenQueued = true;
        Serial.println("FLASH long press; IP screen queued");
      } else {
        storedPhotoQueued = true;
        Serial.println("FLASH short press; stored photo queued");
      }
    }
  }

  const uint32_t heldMs = millis() - buttonPressedAt;
  if (buttonWasPressed && buttonStableState == LOW && !buttonIpIndicated &&
      heldMs >= BUTTON_IP_PRESS_MS) {
    buttonIpIndicated = true;
    digitalWrite(LED_BUILTIN, LOW);
    Serial.println("FLASH IP threshold reached");
  }
  if (buttonWasPressed && buttonStableState == LOW && !buttonClearIndicated &&
      heldMs >= BUTTON_CLEAR_PRESS_MS) {
    buttonClearIndicated = true;
    Serial.println("FLASH clear threshold reached");
  }
  if (buttonWasPressed && buttonClearIndicated) {
    digitalWrite(LED_BUILTIN, ((heldMs / 150U) & 1U) ? LOW : HIGH);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("ESP8266 EPD photo uploader boot");

  pinMode(PIN_BUSY, INPUT);
  pinMode(PIN_RESET, OUTPUT);
  pinMode(PIN_DC, OUTPUT);
  pinMode(PIN_CS, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);

  digitalWrite(PIN_CS, HIGH);
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_RESET, HIGH);
  digitalWrite(LED_BUILTIN, HIGH);

  SPI.begin();
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));

  fileSystemReady = LittleFS.begin();
  if (!fileSystemReady) {
    Serial.println("Formatting LittleFS");
    fileSystemReady = LittleFS.format() && LittleFS.begin();
  }
  Serial.println(fileSystemReady ? "LittleFS ready" : "LittleFS unavailable");

  startNetwork();
  startWebServer();
  tcpServer.begin();
  tcpServer.setNoDelay(true);
  Serial.print("TCP control port: ");
  Serial.println(TCP_CONTROL_PORT);

  const bool screenAlreadyInitialized =
      fileSystemReady && LittleFS.exists(SCREEN_MARKER_PATH);
  if (!screenAlreadyInitialized) {
    drawIpFrame();
    uint32_t setupRefreshMs = 0;
    if (refreshDisplay(&setupRefreshMs)) {
      writeScreenMarker();
      Serial.println("Initial IP screen displayed");
    } else {
      Serial.println("ERROR initial IP screen failed");
    }
  } else {
    Serial.println("Retained screen left unchanged");
  }
}

void loop() {
  if (accessPointMode) {
    dnsServer.processNextRequest();
  }
  if (mdnsReady) {
    MDNS.update();
  }
  server.handleClient();
  pollTcpServer();
  pollButton();

  if (refreshQueued) {
    refreshQueued = false;
    displayState = DisplayState::Refreshing;
    digitalWrite(LED_BUILTIN, LOW);
    lastRefreshMs = 0;
    const bool ok = refreshDisplay(&lastRefreshMs);
    displayState = ok ? DisplayState::Done : DisplayState::Error;
    if (ok) {
      writeScreenMarker();
      Serial.println("PHOTO DISPLAY DONE");
    } else {
      Serial.println("PHOTO DISPLAY FAILED");
    }
    digitalWrite(LED_BUILTIN, HIGH);
  } else if (clearScreenQueued) {
    clearScreenQueued = false;
    displayState = DisplayState::Refreshing;
    digitalWrite(LED_BUILTIN, LOW);
    clearFrame();
    lastRefreshMs = 0;
    const bool ok = refreshDisplay(&lastRefreshMs);
    displayState = ok ? DisplayState::Done : DisplayState::Error;
    Serial.println(ok ? "CLEAR SCREEN DONE" : "CLEAR SCREEN FAILED");
    digitalWrite(LED_BUILTIN, HIGH);
  } else if (storedPhotoQueued) {
    storedPhotoQueued = false;
    displayState = DisplayState::Refreshing;
    digitalWrite(LED_BUILTIN, LOW);
    lastRefreshMs = 0;
    const bool loaded = loadPhoto();
    const bool ok = loaded && refreshDisplay(&lastRefreshMs);
    displayState = ok ? DisplayState::Done : DisplayState::Error;
    if (!loaded) {
      Serial.println("STORED PHOTO NOT FOUND");
    } else {
      Serial.println(ok ? "STORED PHOTO DISPLAY DONE"
                        : "STORED PHOTO DISPLAY FAILED");
    }
    digitalWrite(LED_BUILTIN, HIGH);
  } else if (ipScreenQueued) {
    ipScreenQueued = false;
    displayState = DisplayState::Refreshing;
    digitalWrite(LED_BUILTIN, LOW);
    drawIpFrame();
    lastRefreshMs = 0;
    const bool ok = refreshDisplay(&lastRefreshMs);
    displayState = ok ? DisplayState::Done : DisplayState::Error;
    Serial.println(ok ? "IP SCREEN DONE" : "IP SCREEN FAILED");
    digitalWrite(LED_BUILTIN, HIGH);
  }

  delay(1);
}
