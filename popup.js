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
    let req = new XMLHttpRequest();
    req.open("POST", "https://graph.facebook.com/v2.7/me/live_videos?access_token=" + tok);
    req.addEventListener('load', onLiveVideo);
    req.send();
  }
}

function onLiveVideo() {
  let resp_obj = JSON.parse(this.response);
  console.log(resp_obj);
  let stream_url = resp_obj['stream_url'];
  if (stream_url !== undefined) {
    document.getElementById('url').value = stream_url;
  }
}

function init() {
  document.getElementById('go').addEventListener('click', goOnClick);
  let wv = document.getElementById('wv');
  wv.addEventListener('contentload', wvOnInitialLoad);
  wv.addEventListener('loadredirect', wvOnRedirect);
  wv.src = "https://www.facebook.com/dialog/oauth?client_id=1072171686192806&scope=publish_actions&redirect_uri=https://www.facebook.com/connect/login_success.html";
}

// This function is called by common.js when the NaCl module is
// loaded.
function moduleDidLoad() {
  common.hideModule();
}

// This function is called by common.js when a message is received from the
// NaCl module.
function handleMessage(message) {
  if (message.type === 'message') {
    console.log('nacl', message.data);
  } else {
    console.log(message);
  }
}

document.addEventListener('DOMContentLoaded', init);
