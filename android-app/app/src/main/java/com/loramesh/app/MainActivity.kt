package com.loramesh.app

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.viewmodel.compose.viewModel
import com.loramesh.app.ble.ConnectionState
import com.loramesh.app.ui.MeshViewModel
import com.loramesh.app.ui.screens.*
import com.loramesh.app.ui.theme.*

class MainActivity : ComponentActivity() {

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { /* permissions handled */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestBlePermissions()

        setContent {
            LoRaMeshTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    MainApp()
                }
            }
        }
    }

    private fun requestBlePermissions() {
        val perms = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            perms.add(Manifest.permission.BLUETOOTH_SCAN)
            perms.add(Manifest.permission.BLUETOOTH_CONNECT)
        }
        perms.add(Manifest.permission.ACCESS_FINE_LOCATION)

        val needed = perms.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (needed.isNotEmpty()) {
            permissionLauncher.launch(needed.toTypedArray())
        }
    }
}

// ─── Navigation ──────────────────────────────────────────────

enum class Screen(val label: String, val icon: ImageVector) {
    CHAT("Chat", Icons.Default.Chat),
    NODES("Nodes", Icons.Default.CellTower),
    CONTROL("Control", Icons.Default.SettingsRemote),
    SETTINGS("Settings", Icons.Default.Settings)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainApp(viewModel: MeshViewModel = viewModel()) {
    val connState by viewModel.connectionState.collectAsState()
    val deviceName by viewModel.connectedDeviceName.collectAsState()
    var currentScreen by remember { mutableStateOf(Screen.CHAT) }

    // Show scan screen if not connected
    if (connState != ConnectionState.CONNECTED) {
        ScanScreen(viewModel)
        return
    }

    // Request status and nodes on first connect
    LaunchedEffect(Unit) {
        viewModel.requestStatus()
        viewModel.refreshPins()
        viewModel.requestNodes()
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text("TiggyOpenMesh", style = MaterialTheme.typography.titleMedium)
                        Text(
                            deviceName,
                            style = MaterialTheme.typography.bodySmall,
                            color = MeshGreen
                        )
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface
                ),
                actions = {
                    // Connection indicator
                    Icon(
                        Icons.Default.BluetoothConnected,
                        contentDescription = null,
                        tint = MeshGreen,
                        modifier = Modifier.size(20.dp)
                    )
                    Spacer(Modifier.width(8.dp))
                    // Disconnect button
                    IconButton(onClick = { viewModel.disconnect() }) {
                        Icon(
                            Icons.Default.LinkOff,
                            contentDescription = "Disconnect",
                            tint = MeshRed
                        )
                    }
                }
            )
        },
        bottomBar = {
            NavigationBar(containerColor = MaterialTheme.colorScheme.surface) {
                Screen.entries.forEach { screen ->
                    NavigationBarItem(
                        icon = { Icon(screen.icon, contentDescription = screen.label) },
                        label = { Text(screen.label) },
                        selected = currentScreen == screen,
                        onClick = { currentScreen = screen },
                        colors = NavigationBarItemDefaults.colors(
                            selectedIconColor = MeshCyan,
                            selectedTextColor = MeshCyan,
                            indicatorColor = MeshCyan.copy(alpha = 0.1f)
                        )
                    )
                }
            }
        }
    ) { padding ->
        Box(modifier = Modifier.padding(padding)) {
            when (currentScreen) {
                Screen.CHAT -> ChatScreen(viewModel)
                Screen.NODES -> NodesScreen(viewModel)
                Screen.CONTROL -> ControlScreen(viewModel)
                Screen.SETTINGS -> SettingsScreen(viewModel)
            }
        }
    }
}
