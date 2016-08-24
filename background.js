chrome.app.runtime.onLaunched.addListener(function() {
  chrome.app.window.create('popup.html', {
    'outerBounds': {
      'width': 600,
      'height': 600
    }
  });
});
