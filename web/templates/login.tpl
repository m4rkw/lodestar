<!DOCTYPE html>
<html>
  <head>
    <title>Tracking — login</title>
    <meta name="viewport" content="initial-scale=1.0">
    <meta charset="utf-8">
    <link rel="stylesheet" type="text/css" href="/css/style.css" />
    <script src="/js/jquery.min.js" type="text/javascript"></script>
    <script src="/js/login.js" type="text/javascript"></script>
  </head>
  <body>
    <div class="login-form">
      <form method="post" action="#">
        <table>
          <tr>
            <td>Username:</td>
            <td><input type="text" id="username" name="username" autocomplete="username" /></td>
          </tr>
          <tr>
            <td></td>
            <td><input type="submit" name="login" value="login" /></td>
          </tr>
          <tr>
            <td></td>
            <td><span id="message" style="display:none;color:#c00"></span></td>
          </tr>
        </table>
      </form>
    </div>
  </body>
</html>
