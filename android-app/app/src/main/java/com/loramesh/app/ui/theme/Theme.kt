package com.loramesh.app.ui.theme

import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

// ─── LoRa Mesh colour scheme (matches firmware dark theme) ───
val MeshCyan = Color(0xFF00E5FF)
val MeshGreen = Color(0xFF00E676)
val MeshDarkBg = Color(0xFF0A0E14)
val MeshCardBg = Color(0xFF141A22)
val MeshGrey = Color(0xFF8892A0)
val MeshRed = Color(0xFFFF5252)
val MeshOrange = Color(0xFFFF9100)
val MeshBlue = Color(0xFF448AFF)

private val DarkColorScheme = darkColorScheme(
    primary = MeshCyan,
    secondary = MeshGreen,
    background = MeshDarkBg,
    surface = MeshCardBg,
    error = MeshRed,
    onPrimary = Color.Black,
    onSecondary = Color.Black,
    onBackground = Color.White,
    onSurface = Color.White,
    onError = Color.White,
)

@Composable
fun LoRaMeshTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = DarkColorScheme,
        content = content
    )
}
