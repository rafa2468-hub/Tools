plugins {
    id("com.android.application")
    id("kotlin-android")
    id("dev.flutter.flutter-gradle-plugin")
}

android {
    namespace = "com.paperlessscanner.paperless_scanner"
    compileSdk = 35
    ndkVersion = flutter.ndkVersion

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }

    // Match JVM target to what Flutter's Gradle plugin configures internally (17).
    // Uses the KGP 1.x API (valid through KGP 2.0 as a deprecated-but-functional path).
    kotlinOptions {
        jvmTarget = "17"
    }

    defaultConfig {
        applicationId = "com.paperlessscanner.paperless_scanner"
        minSdk = 29
        targetSdk = 35
        versionCode = flutter.versionCode
        versionName = flutter.versionName
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}

flutter {
    source = "../.."
}
