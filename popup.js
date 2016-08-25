let fb_at = null;

function goOnClick() {
  chrome.desktopCapture.chooseDesktopMedia(
    ["screen", "window", "tab"],
    null,
    function(streamId) {
      let constraints = {
        video: {
          mandatory: {
            chromeMediaSource: "desktop",
            chromeMediaSourceId: streamId,
            maxWidth: screen.width,
            maxHeight: screen.height
          }
        }
      }
      navigator.webkitGetUserMedia(constraints,
        function(stream) {
          common.naclModule.postMessage({
            command: "stream",
            video_track: stream.getVideoTracks()[0],
            url: document.getElementById('url').value
          });
        },
        function(err) {
          console.log('nay');
          console.error(err);
        });
    }
  );
}

function wvOnInitialLoad() {
  let wv = document.getElementById('wv');
  gotNewUrl(wv.src);
}

function wvOnRedirect(e) {
  gotNewUrl(e.newUrl);
}

function gotNewUrl(url) {
  let a = document.createElement('a');
  a.href = url;
  if (a.hostname !== "www.facebook.com" ||
      a.pathname !== "/connect/login_success.html") {
    return;
  }
  let wv = document.getElementById('wv');
  wv.remove();

  code = a.search.slice(6);
  let req = new XMLHttpRequest();
  req.addEventListener('load', gotAccessToken);
  path = "https://graph.facebook.com/v2.7/oauth/access_token";
  client_id = "?client_id=1072171686192806";
  redirect_uri = "&redirect_uri=https://www.facebook.com/connect/login_success.html";
  client_secret = "&client_secret=" + FB_CLIENT_SECRET;
  code = "&code=" + code;
  req.open("GET", path + client_id + redirect_uri + client_secret + code);
  req.send();
}

function gotAccessToken() {
  let resp_obj = JSON.parse(this.response);
  let tok = resp_obj['access_token'];
  if (tok !== undefined) {
    fb_at = tok;
    let req = new XMLHttpRequest();
    req.open("POST", "https://graph.facebook.com/v2.7/me/live_videos");
    req.addEventListener('load', onLiveVideo);
    let params = "access_token=" + fb_at;
    req.send(params);
  }
}

function onLiveVideo() {
  let resp_obj = JSON.parse(this.response);
  console.log(resp_obj);
  let stream_url = resp_obj['stream_url'];
  if (stream_url !== undefined) {
    document.getElementById('url').value = stream_url;
    let stream_id = resp_obj['id'];
    let req = new XMLHttpRequest();
    let params = "access_token="+fb_at+"&fields=preview_url";
    req.open("GET", "https://graph.facebook.com/v2.7/" + stream_id + "?" + params);
    req.addEventListener('load', onStreamDetails);
    req.send();
  }
}

function onStreamDetails() {
  let resp_obj = JSON.parse(this.response);
  console.log('stream details', resp_obj);
  let debug_info = document.getElementById('debug_info');
  debug_info.value = resp_obj['preview_url'];
  debug_info.style.visibility = "visible";

}

function init() {
  document.getElementById('go').addEventListener('click', goOnClick);
  let wv = document.getElementById('wv');
  wv.addEventListener('contentload', wvOnInitialLoad);
  wv.addEventListener('loadredirect', wvOnRedirect);
  wv.src = "https://www.facebook.com/dialog/oauth?client_id=1072171686192806&scope=publish_actions&redirect_uri=https://www.facebook.com/connect/login_success.html";
}

function avLog(msg) {
  let log = document.getElementById('av_log');
  log.style.visibility = "visible";
  log.value += msg;
  log.scrollTop = log.scrollHeight;
}

// This function is called by common.js when the NaCl module is
// loaded.
function moduleDidLoad() {
  common.hideModule();
}

// This function is called by common.js when a message is received from the
// NaCl module.
function handleMessage(message) {
  if (message.type !== 'message') {
    console.log(message);
    return;
  }
  if (message.data.type === undefined) {
    console.log('nacl', message.data);
    return;
  }
  if (message.data.type === 'init') {
    init();
    return;
  }
  if (message.data.type === 'av_log') {
    avLog(message.data.message);
    return;
  }
  if (message.data.type === 'log') {
    console.log(message.data.message);
    return;
  }
}
