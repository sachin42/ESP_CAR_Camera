#ifndef HOMEPAGE_H
#define HOMEPAGE_H

const char *htmlHomePage = R"HTMLHOMEPAGE(
<!DOCTYPE html>
<html>

<head>
  <meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=yes">
  <style>
    .arrows {
      font-size: 40px;
      color: red;
    }

    td.button {
      background-color: black;
      border-radius: 25%;
      box-shadow: 5px 5px #888888;
    }

    td.button:active {
      transform: translate(5px, 5px);
      box-shadow: none;
    }

    .noselect {
      -webkit-touch-callout: none;
      /* iOS Safari */
      -webkit-user-select: none;
      /* Safari */
      -khtml-user-select: none;
      /* Konqueror HTML */
      -moz-user-select: none;
      /* Firefox */
      -ms-user-select: none;
      /* Internet Explorer/Edge */
      user-select: none;
      /* Non-prefixed version */
    }

    .slidecontainer {
      width: 100%;
    }

    .slider {
      -webkit-appearance: none;
      width: 100%;
      height: 15px;
      border-radius: 5px;
      background: #d3d3d3;
      outline: none;
      opacity: 0.7;
      -webkit-transition: .2s;
      transition: opacity .2s;
    }

    .slider:hover {
      opacity: 1;
    }

    .slider::-webkit-slider-thumb {
      -webkit-appearance: none;
      appearance: none;
      width: 25px;
      height: 25px;
      border-radius: 50%;
      background: red;
      cursor: pointer;
    }

    .slider::-moz-range-thumb {
      width: 25px;
      height: 25px;
      border-radius: 50%;
      background: red;
      cursor: pointer;
    }
  </style>
  <title>Wi-Fi Camera</title>
</head>

<body class="noselect" align="center" style="background-color:white">
  <h2 style="color: teal;text-align:center;">Wi-Fi Camera &#128247; </h2>
  <table id="mainTable" style="width:640px;margin:auto;table-layout:fixed" CELLSPACING=10>
    <tr>
      <img id="video" src="" style="width:640px;height:480px"></td>
    </tr>
    <tr />
    <tr />
    <tr>
      <td style="text-align:left"><b>Light:</b></td>
      <td colspan=2>
        <div class="slidecontainer">
          <input type="range" min="0" max="255" value="0" class="slider" id="Light" oninput='Control("Light",value)'>
        </div>
      </td>
    </tr>
  </table>

  <script>
    const ws = new WebSocket(`ws://${window.location.hostname}/stream`);
    ws.binaryType = "blob";
    ws.onmessage = function (event) {
      const imgElement = document.getElementById("video");
      imgElement.src = URL.createObjectURL(event.data);
    };

    ws.onclose = function () {
      console.error("WebSocket closed");
    };

    function Control(key, value) {
      try {
        var xhr = new XMLHttpRequest();
        xhr.open("POST", "/control", true);
        xhr.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");

        xhr.onreadystatechange = function () {
          if (xhr.readyState === 4 && xhr.status === 200) {
            console.log("Control response: " + xhr.responseText);
          }
        };

        xhr.onerror = function () {
          console.error("Error sending control value");
        };

        xhr.send(key + "=" + value);
        console.log("Sending control value: " + value);
      } catch (error) {
        console.error("Error in Control function: ", error);
      }
    }
  </script>
</body>

</html>
)HTMLHOMEPAGE";

#endif