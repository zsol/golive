// Copyright (c) 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Get the current URL.
 *
 * @param {function(string)} callback - called when the URL of the current tab
 *   is found.
 */
function getCurrentTabUrl(callback) {
  // Query filter to be passed to chrome.tabs.query - see
  // https://developer.chrome.com/extensions/tabs#method-query
  var queryInfo = {
    active: true,
    currentWindow: true
  };

  chrome.tabs.query(queryInfo, function(tabs) {
    // chrome.tabs.query invokes the callback with a list of tabs that match the
    // query. When the popup is opened, there is certainly a window and at least
    // one tab, so we can safely assume that |tabs| is a non-empty array.
    // A window can only have one active tab at a time, so the array consists of
    // exactly one tab.
    var tab = tabs[0];

    // A tab is a plain object that provides information about the tab.
    // See https://developer.chrome.com/extensions/tabs#type-Tab
    var url = tab.url;

    // tab.url is only available if the "activeTab" permission is declared.
    // If you want to see the URL of other tabs (e.g. after removing active:true
    // from |queryInfo|), then the "tabs" permission is required to see their
    // "url" properties.
    console.assert(typeof url == 'string', 'tab.url should be a string');

    callback(url);
  });

  // Most methods of the Chrome extension APIs are asynchronous. This means that
  // you CANNOT do something like this:
  //
  // var url;
  // chrome.tabs.query(queryInfo, function(tabs) {
  //   url = tabs[0].url;
  // });
  // alert(url); // Shows "undefined", because chrome.tabs.query is async.
}

function renderStatus(statusText) {
  document.getElementById('status').textContent = statusText;
}

function goOnClick() {
  console.log('clicked');
  chrome.desktopCapture.chooseDesktopMedia(
    ["screen", "window", "tab"],
    null,
    function(streamId) {
      let constraints = {
        video: {
          mandatory: {
            chromeMediaSource: "desktop",
            chromeMediaSourceId: streamId
          }
        }
      }
      navigator.webkitGetUserMedia(constraints,
        function(stream) {
          console.log('yay');
          console.log(stream);
          let video = document.createElement('video');
          video.src = window.URL.createObjectURL(stream);
          document.getElementById('video').appendChild(video);
          common.naclModule.postMessage({
            command: "stream",
            video_track: stream.getVideoTracks()[0]
          });
        },
        function(err) {
          console.log('nay');
          console.error(err);
        });
    }
  );
}

function init() {
  document.getElementById('go').addEventListener('click', goOnClick);
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
