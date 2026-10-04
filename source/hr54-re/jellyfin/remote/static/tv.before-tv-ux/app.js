(function () {
  var state = document.getElementById("state");
  var libraries = document.getElementById("libraries");
  var items = document.getElementById("items");
  var heading = document.getElementById("heading");

  function request(method, url, body, done) {
    var xhr = new XMLHttpRequest();
    xhr.open(method, url, true);
    xhr.setRequestHeader("Content-Type", "application/json");
    xhr.onreadystatechange = function () {
      if (xhr.readyState !== 4) return;
      if (xhr.status >= 200 && xhr.status < 300) done(JSON.parse(xhr.responseText));
      else state.innerHTML = "Request failed: " + xhr.status;
    };
    xhr.send(body ? JSON.stringify(body) : null);
  }

  function telemetry(type, event) {
    var payload = JSON.stringify({
      type: type,
      keyCode: event && (event.keyCode || event.which) || 0,
      key: event && event.key || ""
    });
    var request = new XMLHttpRequest();
    request.open("POST", "/api/tv/event", true);
    request.setRequestHeader("Content-Type", "application/json");
    request.send(payload);
  }

  function loadItems(parent, name) {
    state.innerHTML = "Loading " + name + "…";
    request("GET", "/api/items?parent=" + encodeURIComponent(parent), null, function (data) {
      heading.innerHTML = name;
      items.innerHTML = "";
      var list = data.items || [];
      for (var i = 0; i < list.length && i < 12; i++) {
        (function (item) {
          var button = document.createElement("button");
          button.innerHTML = item.name;
          button.onclick = function () {
            if (!item.playable) return;
            state.innerHTML = "Starting " + item.name + "…";
            request("POST", "/api/play", {itemId:item.id}, function () {
              state.innerHTML = "Playing " + item.name;
            });
          };
          items.appendChild(button);
        }(list[i]));
      }
      state.innerHTML = list.length + " titles";
      if (items.firstChild) items.firstChild.focus();
    });
  }

  request("GET", "/api/libraries", null, function (data) {
    var list = data.libraries || [];
    for (var i = 0; i < list.length; i++) {
      (function (library) {
        var button = document.createElement("button");
        button.innerHTML = library.name;
        button.onclick = function () { loadItems(library.id, library.name); };
        libraries.appendChild(button);
      }(list[i]));
    }
    state.innerHTML = "Choose a library";
    if (libraries.firstChild) libraries.firstChild.focus();
  });

  document.onkeydown = function (event) {
    event = event || window.event;
    telemetry("keydown", event);
    if ((event.keyCode || event.which) === 83) {
      request("POST", "/api/tv/exit", {}, function () {});
    }
  };
  document.onkeypress = function (event) { telemetry("keypress", event || window.event); };
  document.onkeyup = function (event) { telemetry("keyup", event || window.event); };
  telemetry("load", null);
}());
