{
  description = "Overtone Mandelbrot: Android build environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
        config = {
          allowUnfree = true;
          android_sdk.accept_license = true;
        };
      };

      # Must match ndkVersion in app/build.gradle and cmake.version there.
      ndkVersion = "26.1.10909125";
      cmakeVersion = "3.30.3"; # vendored raylib needs >= 3.25

      android = pkgs.androidenv.composeAndroidPackages {
        platformVersions = [ "34" ];
        buildToolsVersions = [ "34.0.0" ];
        includeNDK = true;
        ndkVersions = [ ndkVersion ];
        cmakeVersions = [ cmakeVersion ];
        includeEmulator = false;
        includeSystemImages = false;
      };
      sdk = "${android.androidsdk}/libexec/android-sdk";
    in {
      devShells.${system} = {
      default = pkgs.mkShell {
        packages = [ pkgs.jdk17 android.androidsdk android.platform-tools ];
        ANDROID_HOME = sdk;
        ANDROID_SDK_ROOT = sdk;
        JAVA_HOME = pkgs.jdk17.home;
        # aapt2 from Maven is dynamically linked and won't run on NixOS;
        # use the one from the SDK instead.
        GRADLE_OPTS = "-Dorg.gradle.project.android.aapt2FromMavenOverride=${sdk}/build-tools/34.0.0/aapt2";
      };

      # Desktop build: nix develop .#desktop
      desktop =
        let
          # raylib/GLFW and miniaudio dlopen these at runtime
          runtimeLibs = with pkgs; [
            libGL libx11 libxrandr libxinerama libxcursor libxi
            libxkbcommon wayland alsa-lib libpulseaudio
          ];
        in pkgs.mkShell {
          packages = with pkgs; [ cmake gcc git pkg-config wayland-scanner ffmpeg qrencode ] ++ runtimeLibs;
          LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath runtimeLibs;
        };
      };
    };
}
