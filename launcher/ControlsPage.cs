using System.ComponentModel;

namespace Hajinmon.Launcher;

/// <summary>
/// "Controls" tab: controller remapping (padbind_* options) and keyboard/mouse
/// bindings (keybind_* options).
/// </summary>
public sealed class ControlsPage : TabPage
{
    /// <summary>A guest controller input and the game options that bind it.</summary>
    private sealed record Binding(string Label, string? PadOption, string? PadDefault,
                                  string KeyOption, string KeyDefault);

    // Defaults match the game (doa4/src/input_defaults.cpp).
    private static readonly Binding[] Bindings =
    {
        new("A", "padbind_a", "A", "keybind_a", "S"),
        new("B", "padbind_b", "B", "keybind_b", "D"),
        new("X", "padbind_x", "X", "keybind_x", "A"),
        new("Y", "padbind_y", "Y", "keybind_y", "W"),
        new("LB", "padbind_left_shoulder", "LB", "keybind_left_shoulder", "1"),
        new("RB", "padbind_right_shoulder", "RB", "keybind_right_shoulder", "3"),
        new("LT", "padbind_left_trigger", "LT", "keybind_left_trigger", "Q"),
        new("RT", "padbind_right_trigger", "RT", "keybind_right_trigger", "E"),
        new("Start", "padbind_start", "Start", "keybind_start", "Return"),
        new("Back", "padbind_back", "Back", "keybind_back", "Z,Tab"),
        new("D-pad up", "padbind_dpad_up", "DpadUp", "keybind_dpad_up", "Up"),
        new("D-pad down", "padbind_dpad_down", "DpadDown", "keybind_dpad_down", "Down"),
        new("D-pad left", "padbind_dpad_left", "DpadLeft", "keybind_dpad_left", "Left"),
        new("D-pad right", "padbind_dpad_right", "DpadRight", "keybind_dpad_right", "Right"),
        new("Left stick up", null, null, "keybind_lstick_up", "Up"),
        new("Left stick down", null, null, "keybind_lstick_down", "Down"),
        new("Left stick left", null, null, "keybind_lstick_left", "Left"),
        new("Left stick right", null, null, "keybind_lstick_right", "Right"),
        new("Left stick press", "padbind_lstick_press", "LS", "keybind_lstick_press", "F"),
        new("Right stick up", null, null, "keybind_rstick_up", ""),
        new("Right stick down", null, null, "keybind_rstick_down", ""),
        new("Right stick left", null, null, "keybind_rstick_left", ""),
        new("Right stick right", null, null, "keybind_rstick_right", ""),
        new("Right stick press", "padbind_rstick_press", "RS", "keybind_rstick_press", "K"),
    };

    private static readonly (string Label, string Value)[] PadControls =
    {
        ("A", "A"), ("B", "B"), ("X", "X"), ("Y", "Y"),
        ("LB", "LB"), ("RB", "RB"), ("LT", "LT"), ("RT", "RT"),
        ("Start", "Start"), ("Back", "Back"),
        ("D-pad up", "DpadUp"), ("D-pad down", "DpadDown"),
        ("D-pad left", "DpadLeft"), ("D-pad right", "DpadRight"),
        ("Left stick press", "LS"), ("Right stick press", "RS"),
        ("(none)", "None"),
    };

    private readonly Dictionary<string, ComboBox> _padBoxes = new();
    private readonly Dictionary<string, KeyBindButton> _keyButtons = new();
    private readonly CheckBox _keyboard = new() { Text = "Enable keyboard and mouse", AutoSize = true };
    private readonly CheckBox _mouseLook = new() { Text = "Mouse moves the right stick", AutoSize = true };
    private readonly NumericUpDown _sensitivity = new()
    {
        Minimum = 0.1m, Maximum = 10m, DecimalPlaces = 1, Increment = 0.1m, Width = 70,
    };

    public event EventHandler? Changed;

    public ControlsPage() : base("Controls")
    {
        var grid = new TableLayoutPanel
        {
            Dock = DockStyle.Fill, Padding = new Padding(10, 6, 10, 6), ColumnCount = 3, AutoScroll = true,
        };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

        var options = new FlowLayoutPanel { AutoSize = true, WrapContents = true, Margin = new Padding(0, 0, 0, 4) };
        _keyboard.Margin = new Padding(3, 4, 12, 0);
        _mouseLook.Margin = new Padding(3, 4, 6, 0);
        var sensitivityLabel = new Label { Text = "Sensitivity", AutoSize = true, Margin = new Padding(3, 6, 3, 0) };
        options.Controls.AddRange(new Control[] { _keyboard, _mouseLook, sensitivityLabel, _sensitivity });
        _keyboard.CheckedChanged += (_, _) => UpdateEnabled();
        _mouseLook.CheckedChanged += (_, _) => UpdateEnabled();
        AddSpanning(grid, options);

        AddHeader(grid, "Xbox 360", "Controller", "Keyboard / mouse");
        foreach (var binding in Bindings)
        {
            int row = grid.RowCount++;
            grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            grid.Controls.Add(new Label
            {
                Text = binding.Label, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(3, 6, 10, 0),
            }, 0, row);

            if (binding.PadOption != null)
            {
                var combo = new ComboBox
                {
                    DropDownStyle = ComboBoxStyle.DropDownList, Width = 130, Margin = new Padding(3, 2, 8, 2),
                };
                foreach (var control in PadControls) combo.Items.Add(control.Label);
                combo.SelectedIndexChanged += (_, _) => Changed?.Invoke(this, EventArgs.Empty);
                _padBoxes[binding.PadOption] = combo;
                grid.Controls.Add(combo, 1, row);
            }
            else
            {
                grid.Controls.Add(new Label
                {
                    Text = "left stick", AutoSize = true, ForeColor = SystemColors.GrayText,
                    Anchor = AnchorStyles.Left, Margin = new Padding(6, 6, 8, 0),
                }, 1, row);
            }

            var button = new KeyBindButton(binding.KeyDefault) { Width = 190, Height = 27, Margin = new Padding(3, 2, 3, 2) };
            button.BindingChanged += (_, _) => Changed?.Invoke(this, EventArgs.Empty);
            _keyButtons[binding.KeyOption] = button;
            grid.Controls.Add(button, 2, row);
        }

        AddSpanning(grid, new Label
        {
            Text = "Click a keyboard binding and press a key or mouse button (Esc cancels). Right-click it " +
                   "to add an alternative key, clear it or reset it. Avoid ` and F4: the game uses them for " +
                   "its console and settings.",
            AutoSize = true, MaximumSize = new Size(440, 0), ForeColor = SystemColors.GrayText,
            Margin = new Padding(3, 8, 3, 3),
        });
        var reset = new LinkLabel { Text = "Reset all controls to default", AutoSize = true };
        reset.LinkClicked += (_, _) => ResetDefaults();
        AddSpanning(grid, reset);

        Controls.Add(grid);
    }

    private static void AddSpanning(TableLayoutPanel grid, Control control)
    {
        int row = grid.RowCount++;
        grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        grid.Controls.Add(control, 0, row);
        grid.SetColumnSpan(control, 3);
    }

    private static void AddHeader(TableLayoutPanel grid, params string[] titles)
    {
        int row = grid.RowCount++;
        grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        for (int i = 0; i < titles.Length; i++)
        {
            var label = new Label
            {
                Text = titles[i], AutoSize = true, Margin = new Padding(3, 6, 3, 2),
            };
            label.Font = new Font(label.Font, FontStyle.Bold);
            grid.Controls.Add(label, i, row);
        }
    }

    private void UpdateEnabled()
    {
        foreach (var button in _keyButtons.Values) button.Enabled = _keyboard.Checked;
        _mouseLook.Enabled = _keyboard.Checked;
        _sensitivity.Enabled = _keyboard.Checked && _mouseLook.Checked;
        Changed?.Invoke(this, EventArgs.Empty);
    }

    private void ResetDefaults()
    {
        foreach (var binding in Bindings)
        {
            if (binding.PadOption != null) SelectPad(binding.PadOption, binding.PadDefault!);
            _keyButtons[binding.KeyOption].Value = binding.KeyDefault;
        }
        _mouseLook.Checked = false;
        _sensitivity.Value = 1.0m;
    }

    private void SelectPad(string option, string value)
    {
        int index = Array.FindIndex(PadControls, c => c.Value == value);
        _padBoxes[option].SelectedIndex = index >= 0 ? index : PadControls.Length - 1;
    }

    public void LoadFrom(Settings s)
    {
        foreach (var binding in Bindings)
        {
            if (binding.PadOption != null)
            {
                SelectPad(binding.PadOption,
                          s.PadBinds.TryGetValue(binding.PadOption, out var pad) ? pad : binding.PadDefault!);
            }
            _keyButtons[binding.KeyOption].Value =
                s.KeyBinds.TryGetValue(binding.KeyOption, out var key) ? key : binding.KeyDefault;
        }
        _keyboard.Checked = s.KeyboardControls;
        _mouseLook.Checked = s.MouseLook;
        _sensitivity.Value = Math.Clamp((decimal)s.MouseSensitivity, _sensitivity.Minimum, _sensitivity.Maximum);
        UpdateEnabled();
    }

    public void SaveTo(Settings s)
    {
        s.PadBinds.Clear();
        s.KeyBinds.Clear();
        foreach (var binding in Bindings)
        {
            if (binding.PadOption != null)
            {
                string pad = PadControls[Math.Max(0, _padBoxes[binding.PadOption].SelectedIndex)].Value;
                if (pad != binding.PadDefault) s.PadBinds[binding.PadOption] = pad;
            }
            string key = _keyButtons[binding.KeyOption].Value;
            if (key != binding.KeyDefault) s.KeyBinds[binding.KeyOption] = key;
        }
        s.KeyboardControls = _keyboard.Checked;
        s.MouseLook = _mouseLook.Checked;
        s.MouseSensitivity = (double)_sensitivity.Value;
    }

    /// <summary>Command line options for the bindings that differ from the defaults.</summary>
    public static IEnumerable<string> Arguments(Settings s)
    {
        foreach (var (option, value) in s.PadBinds) yield return $"--{option}={value}";
        if (!s.KeyboardControls) yield break;
        foreach (var (option, value) in s.KeyBinds) yield return $"--{option}={(value.Length == 0 ? "None" : value)}";
        if (s.MouseLook)
        {
            yield return "--mnk_mouse";
            yield return FormattableString.Invariant($"--mnk_sensitivity={s.MouseSensitivity:0.0}");
        }
    }
}

/// <summary>
/// Shows a keyboard binding ("Q,I", "Shift+Up", "LMB"...) and records a new key
/// or mouse button when clicked.
/// </summary>
public sealed class KeyBindButton : Button
{
    private readonly string _default;
    private string _value = "";
    private bool _capturing;
    private bool _append;
    private bool _suppressMenu;

    public event EventHandler? BindingChanged;

    public KeyBindButton(string defaultValue)
    {
        _default = defaultValue;
        TextAlign = ContentAlignment.MiddleLeft;
        var menu = new ContextMenuStrip();
        menu.Items.Add("Add alternative key", null, (_, _) => StartCapture(append: true));
        menu.Items.Add("Clear", null, (_, _) => Value = "");
        menu.Items.Add("Reset to default", null, (_, _) => Value = _default);
        menu.Opening += (_, e) =>
        {
            e.Cancel = _capturing || _suppressMenu;
            _suppressMenu = false;
        };
        ContextMenuStrip = menu;
    }

    [Browsable(false), DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public string Value
    {
        get => _value;
        set
        {
            _value = value;
            UpdateText();
            BindingChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    private void UpdateText()
    {
        Text = _capturing ? "Press a key or mouse button..."
             : _value.Length == 0 ? "(none)"
             : _value.Replace(",", ", ");
    }

    private void StartCapture(bool append)
    {
        _capturing = true;
        _append = append;
        Focus();
        UpdateText();
    }

    private void EndCapture(string? key)
    {
        _capturing = false;
        if (key == null)
        {
            UpdateText();
            return;
        }
        Value = _append && _value.Length > 0 && !_value.Split(',').Contains(key) ? $"{_value},{key}" : key;
    }

    protected override void OnClick(EventArgs e)
    {
        base.OnClick(e);
        if (!_capturing) StartCapture(append: false);
    }

    protected override void OnLostFocus(EventArgs e)
    {
        base.OnLostFocus(e);
        if (_capturing) EndCapture(null);
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        if (_capturing)
        {
            string? button = e.Button switch
            {
                MouseButtons.Left => "LMB",
                MouseButtons.Right => "RMB",
                MouseButtons.Middle => "MMB",
                _ => null,
            };
            if (button != null)
            {
                // Binding the right button must not also open the menu.
                _suppressMenu = e.Button == MouseButtons.Right;
                EndCapture(Modifiers(ModifierKeys) + button);
                return;
            }
        }
        base.OnMouseDown(e);
    }

    protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
    {
        if (!_capturing) return base.ProcessCmdKey(ref msg, keyData);
        Keys key = keyData & Keys.KeyCode;
        if (key == Keys.Escape)
        {
            EndCapture(null);
            return true;
        }
        if (key is Keys.ShiftKey or Keys.ControlKey or Keys.Menu or Keys.LShiftKey or Keys.RShiftKey
                or Keys.LControlKey or Keys.RControlKey or Keys.LMenu or Keys.RMenu)
        {
            return true;  // Wait for the actual key.
        }
        string? name = KeyName(key);
        if (name != null) EndCapture(Modifiers(keyData) + name);
        return true;
    }

    private static string Modifiers(Keys keys)
    {
        string prefix = "";
        if ((keys & Keys.Shift) != 0) prefix += "Shift+";
        if ((keys & Keys.Control) != 0) prefix += "Ctrl+";
        if ((keys & Keys.Alt) != 0) prefix += "Alt+";
        return prefix;
    }

    /// <summary>Key names understood by the game runtime.</summary>
    private static string? KeyName(Keys key)
    {
        if (key is >= Keys.A and <= Keys.Z) return key.ToString();
        if (key is >= Keys.D0 and <= Keys.D9) return ((char)('0' + (key - Keys.D0))).ToString();
        if (key is >= Keys.F1 and <= Keys.F24) return key.ToString();
        if (key is >= Keys.NumPad0 and <= Keys.NumPad9) return $"Numpad{key - Keys.NumPad0}";
        return key switch
        {
            Keys.Oemtilde => "Backtick",
            Keys.OemMinus => "Minus",
            Keys.Oemplus => "Plus",
            Keys.Oemcomma => "Comma",
            Keys.OemPeriod => "Period",
            Keys.OemSemicolon => "Semicolon",
            Keys.OemQuestion => "Slash",
            Keys.OemPipe => "Backslash",
            Keys.OemBackslash => "Backslash",
            Keys.OemOpenBrackets => "LBracket",
            Keys.OemCloseBrackets => "RBracket",
            Keys.OemQuotes => "Quote",
            Keys.Return => "Return",
            Keys.Space => "Space",
            Keys.Tab => "Tab",
            Keys.Back => "Backspace",
            Keys.Delete => "Delete",
            Keys.Insert => "Insert",
            Keys.Home => "Home",
            Keys.End => "End",
            Keys.PageUp => "PageUp",
            Keys.PageDown => "PageDown",
            Keys.Left => "Left",
            Keys.Right => "Right",
            Keys.Up => "Up",
            Keys.Down => "Down",
            Keys.Add => "NumpadPlus",
            Keys.Subtract => "NumpadMinus",
            Keys.Multiply => "NumpadStar",
            Keys.Divide => "NumpadSlash",
            Keys.Pause => "Pause",
            Keys.CapsLock => "CapsLock",
            Keys.NumLock => "NumLock",
            Keys.Scroll => "ScrollLock",
            _ => null,
        };
    }
}
