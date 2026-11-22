import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val keystorePropsFile = rootProject.file("keystore.properties")
val keystoreProps = Properties().apply {
    if (keystorePropsFile.exists()) {
        keystorePropsFile.inputStream().use { load(it) }
    }
}

val releaseStorePath = keystoreProps.getProperty("storeFile") ?: System.getenv("ZEROCAM_STORE_FILE")
val releaseStorePass = keystoreProps.getProperty("storePassword") ?: System.getenv("ZEROCAM_STORE_PASSWORD")
val releaseKeyAlias = keystoreProps.getProperty("keyAlias") ?: System.getenv("ZEROCAM_KEY_ALIAS")
val releaseKeyPass = keystoreProps.getProperty("keyPassword") ?: System.getenv("ZEROCAM_KEY_PASSWORD")
val releaseStoreFileObj = releaseStorePath?.takeIf { it.isNotBlank() }?.let { rootProject.file(it) }
val releaseSigningReady = releaseStoreFileObj != null &&
    releaseStoreFileObj.exists() &&
    !releaseStorePass.isNullOrBlank() &&
    !releaseKeyAlias.isNullOrBlank() &&
    !releaseKeyPass.isNullOrBlank()

android {
    namespace = "com.zerocam.mobile"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.zerocam.mobile"
        minSdk = 26
        targetSdk = 35
        versionCode = 7
        versionName = "1.2.4"
    }

    dependenciesInfo {
        includeInApk = false
        includeInBundle = false
    }

    packaging {
        resources {
            excludes += setOf(
                "DebugProbesKt.bin",
                "META-INF/*.version",
                "META-INF/com/android/build/gradle/app-metadata.properties",
                "META-INF/version-control-info.textproto",
                "META-INF/CHANGES",
                "META-INF/README.md",
                "kotlin-tooling-metadata.json",
                "**/*.kotlin_builtins"
            )
        }
    }

    signingConfigs {
        if (releaseSigningReady) {
            create("release") {
                storeFile = releaseStoreFileObj
                storePassword = releaseStorePass
                keyAlias = releaseKeyAlias
                keyPassword = releaseKeyPass
                enableV1Signing = false
                enableV2Signing = true
                enableV3Signing = true
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            vcsInfo {
                include = false
            }
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            if (releaseSigningReady) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    testImplementation("junit:junit:4.13.2")
}
