using System.Globalization;
using System.Text;
using NfaLoader.Localization;
using NfaLoader.Models;
using NfaLoader.Services;

namespace NfaLoader;

// Private stdin/stdout protocol. Fields are base64 UTF-8; credentials never enter
// command lines, temporary request files, or the visible status log.
internal static class NativeBridge
{
    private static readonly object OutputGate = new();
    private static StreamWriter _output = null!;
    private static readonly IProgress<string> Progress = new InlineProgress();

    public static async Task<int> RunAsync()
    {
        using var input = new StreamReader(Console.OpenStandardInput(), Encoding.UTF8);
        using var output = new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false)) { AutoFlush = true };
        _output = output;
        Loc.Initialize(AppState.SettingsService);
        Loc.SetLanguage("en", persist: false);
        AppState.StatusReporter = (message, _) => Send("status", message);
        Send("ready", "NFA services ready (v0.5.5).");
        string? line;
        while ((line = await input.ReadLineAsync()) is not null)
        {
            try
            {
                var fields = line.Split('\t');
                var args = fields.Skip(1).Select(s => Encoding.UTF8.GetString(Convert.FromBase64String(s))).ToArray();
                await ExecuteAsync(fields[0], args);
            }
            catch (Exception ex)
            {
                Send("error", ex is NfaApiException api ? $"{api.Code}: {api.Message}" : ex.Message);
            }
            finally
            {
                Send("done");
            }
        }
        return 0;
    }

    private static void Send(string type, params string?[] fields)
    {
        lock (OutputGate)
            _output.WriteLine(type + string.Concat(fields.Select(s => "\t" + Convert.ToBase64String(Encoding.UTF8.GetBytes(s ?? "")))));
    }

    private sealed class InlineProgress : IProgress<string>
    {
        public void Report(string value) => Send("status", value);
    }

    private static string Arg(string[] args, int i) => i < args.Length ? args[i] : "";
    private static string Key() => AppState.GetNfaApiKey() is { Length: > 0 } key ? key : throw new InvalidOperationException("Save your nfa.pub API key in Settings first.");

    private static (string Token, JwtValidationResult Info, string Name) Credentials(string raw)
    {
        var token = FormatHelper.NormalizeToken(raw.Trim());
        var info = AppState.JwtTokenService.Validate(token);
        var name = AppState.AccountHistoryService.Load().FirstOrDefault(a => a.SteamId == info.SteamId)?.AccountName ?? info.SteamId;
        return (token, info, name);
    }

    private static async Task ExecuteAsync(string command, string[] args)
    {
        switch (command)
        {
            case "hello":
                SendSettings();
                SendHistory();
                SendLoadout();
                break;
            case "inspect":
            {
                var token = FormatHelper.NormalizeToken(Arg(args, 0));
                var info = AppState.JwtTokenService.Inspect(token);
                Send("inspection", info.SteamId, info.ExpiresAt?.ToLocalTime().ToString("yyyy-MM-dd HH:mm"), info.Status, info.IsValid ? "1" : "0");
                Send(info.IsValid ? "status" : "error", info.IsValid ? "Token format and expiry checked locally. Use Verify to check with Steam." : info.Status);
                break;
            }
            case "verify":
            {
                var (token, info, _) = Credentials(Arg(args, 0));
                Send("status", "Checking token with Steam...");
                var online = await AppState.TokenOnlineValidationService.ValidateAsync(token);
                Send("inspection", info.SteamId, info.ExpiresAt.ToLocalTime().ToString("yyyy-MM-dd HH:mm"), online.Status, online.IsValid ? "1" : "0");
                Send(online.IsValid ? "status" : "error", online.Status);
                break;
            }
            case "login":
            {
                var (token, info, name) = Credentials(Arg(args, 0));
                _ = SteamPathCoordinator.ResolvePathsOrThrow();
                Send("status", "Checking token before restarting Steam...");
                var online = await AppState.TokenOnlineValidationService.ValidateAsync(token);
                if (!online.IsValid) throw new InvalidOperationException(online.Status);
                var result = AppState.LoginService.Login(name, token, Progress);
                await AppState.AccountHistoryService.SaveLoginAsync(name, result.SteamId, token, result.ExpiresAt);
                SendHistory();
                Send("status", "Steam launch requested. Account saved to History.");
                break;
            }
            case "history": SendHistory(); break;
            case "cached":
                Send("cached-clear");
                foreach (var account in AppState.LoginService.GetCachedLoginAccounts())
                    Send("cached", account.SteamId, account.AccountName, account.PersonaName);
                break;
            case "restore":
            {
                var account = AppState.LoginService.GetCachedLoginAccounts().FirstOrDefault(a => a.SteamId == Arg(args, 0))
                    ?? throw new InvalidOperationException("Cached account no longer exists. Refresh the list.");
                var automatic = AppState.LoginService.RestoreCachedLogin(account, Progress);
                Send("status", automatic ? "Steam launch requested with the cached login." : "Steam launch requested. Steam may ask you to sign in.");
                break;
            }
            case "import":
            {
                var entries = new List<AccountImportEntry>();
                var lines = Arg(args, 0).Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries);
                foreach (var raw in lines)
                {
                    var (token, info, name) = Credentials(raw);
                    entries.Add(new AccountImportEntry { AccountName = name, EyaToken = token, SteamId = info.SteamId,
                        TokenExpiresAt = info.ExpiresAt, TokenIsValid = true, TokenStatus = "Valid" });
                }
                if (entries.Count == 0) throw new InvalidOperationException("Paste at least one token or steamid----token line.");
                var result = AppState.AccountHistoryService.ImportAccounts(entries);
                SendHistory();
                Send("status", $"Imported {result.Added} accounts; updated {result.Updated}.");
                break;
            }
            case "delete":
            {
                var account = FindAccount(Arg(args, 0));
                AppState.AccountHistoryService.DeleteAccounts([account]);
                SendHistory();
                Send("status", "Account removed from local history.");
                break;
            }
            case "query":
            {
                var (token, info, name) = Credentials(Arg(args, 0));
                Send("status", "Querying CS2 rating, level, cooldown and VAC status...");
                var result = await AppState.PremierScoreService.QueryAsync(token, info.SteamId);
                AppState.AccountHistoryService.SaveCsAccountStatus(name, info.SteamId, token, info.ExpiresAt,
                    result, new SteamTokenOnlineValidationResult(true, "Steam accepted token"));
                SendHistory();
                Send("status", $"Rating: {result.DisplayText} / Level: {result.PlayerLevelText} / VAC: {result.GcVacText} / {result.CooldownStatusText}");
                break;
            }
            case "stock":
            {
                var stock = await AppState.NfaClient.GetCs2StockAsync(AppState.GetNfaApiKey());
                Send("stock-clear");
                foreach (var item in stock)
                    Send("stock", item.ProductId, item.Type, item.Name, NfaPubClient.FormatEur(item.PriceEur), item.Available.ToString());
                if (AppState.HasNfaApiKey)
                    Send("balance", NfaPubClient.FormatEur(await AppState.NfaClient.GetBalanceAsync(Key())));
                Send("status", $"Loaded {stock.Count} CS2 products.");
                break;
            }
            case "buy": await PurchaseAsync(Arg(args, 0), Arg(args, 1), recovering: false); break;
            case "finish-purchase": await PurchaseAsync("", "", recovering: true); break;
            case "clear-pending":
                AppState.ClearPendingPurchase();
                SendSettings();
                Send("status", "Pending purchase record cleared after manual order review.");
                break;
            case "replace": await ReplaceAsync(Arg(args, 0), Arg(args, 1), Arg(args, 2)); break;
            case "settings": SendSettings(); break;
            case "save-settings":
            {
                var settings = AppState.SettingsService.Load();
                var path = Arg(args, 1).Trim();
                if (path.Length > 0 && !SteamPathService.ContainsSteamExe(path))
                    throw new InvalidOperationException("Steam folder must contain steam.exe.");
                if (!string.IsNullOrWhiteSpace(Arg(args, 0)))
                    settings.NfaApiKeyProtected = SecretProtector.Protect(Arg(args, 0).Trim());
                settings.SteamInstallPath = path.Length > 0 ? SteamPathService.NormalizeInstallPath(path) : null;
                settings.SteamInstallPathUserPicked = path.Length > 0;
                if (!AppState.SettingsService.Save(settings)) throw new IOException("Settings could not be saved.");
                SendSettings();
                Send("status", "Settings saved. API key encrypted with Windows DPAPI.");
                break;
            }
            case "remove-key":
            {
                var settings = AppState.SettingsService.Load();
                settings.NfaApiKeyProtected = null;
                if (!AppState.SettingsService.Save(settings)) throw new IOException("Settings could not be saved.");
                SendSettings();
                break;
            }
            case "workshop":
            {
                var (token, _, _) = Credentials(Arg(args, 0));
                var count = await AppState.WorkshopService.ClearSubscriptionsAsync(token, Progress);
                Send("status", $"Unsubscribed from {count} Workshop items.");
                break;
            }
            case "loadout":
            {
                var (token, info, _) = Credentials(Arg(args, 0));
                var result = await AppState.LoadoutService.ApplyPresetAsync(AppState.SettingsService.Load().Loadout, token, info.SteamId);
                Send(result.IsSuccess ? "status" : "error", $"Loadout: {result.Confirmed}/{result.Requested} slots confirmed." +
                    (result.Failures.Count > 0 ? " " + string.Join(" / ", result.Failures) : ""));
                break;
            }
            case "save-loadout":
            {
                var preset = new CsLoadoutPreset();
                foreach (var line in Arg(args, 0).Split(';', StringSplitOptions.RemoveEmptyEntries))
                {
                    var parts = line.Split(',');
                    var ct = parts[0] == "CT";
                    var slot = uint.Parse(parts[1], CultureInfo.InvariantCulture);
                    var def = uint.Parse(parts[2], CultureInfo.InvariantCulture);
                    var weapon = CsWeaponCatalog.All.FirstOrDefault(w => w.Def == def && w.UsableBy(ct))
                        ?? throw new InvalidOperationException("Invalid weapon selection.");
                    var valid = slot == 2 ? weapon.Starter : slot is >= 3 and <= 6 ? weapon.Category == CsWeaponCategory.Pistol && !weapon.Starter :
                        slot is >= 8 and <= 12 ? weapon.Category == CsWeaponCategory.Mid : slot is >= 14 and <= 18 ? weapon.Category == CsWeaponCategory.Rifle :
                        slot == 34 && weapon.Def == 31;
                    if (!valid) throw new InvalidOperationException("Weapon does not fit the selected loadout slot.");
                    var team = ct ? preset.Ct : preset.T;
                    if (team.Values.Contains(def)) throw new InvalidOperationException("A weapon can only appear once per team.");
                    team.Add(slot, def);
                }
                if (preset.T.Count != 15 || preset.Ct.Count != 15) throw new InvalidOperationException("Fill all 15 slots on each team.");
                var settings = AppState.SettingsService.Load();
                settings.Loadout = preset;
                if (!AppState.SettingsService.Save(settings)) throw new IOException("Loadout could not be saved.");
                SendLoadout();
                Send("status", "T and CT loadouts saved.");
                break;
            }
            case "profile":
            {
                var (token, info, _) = Credentials(Arg(args, 0));
                var name = Arg(args, 1).Trim();
                if (name.Length == 0) throw new InvalidOperationException("Enter a nickname first.");
                var result = await AppState.ProfileService.ApplyAsync(token,
                    new SteamProfileApplyRequest(name, null, null, null, false), Progress);
                if (!result.IsFullSuccess) throw new InvalidOperationException(result.ProfileError ?? "Profile could not be updated.");
                AppState.AccountHistoryService.UpdateProfileLocally(info.SteamId, name, null);
                var settings = AppState.SettingsService.Load();
                settings.PersonaName = name;
                AppState.SettingsService.Save(settings);
                SendHistory();
                Send("status", "Steam nickname updated.");
                break;
            }
            default: throw new InvalidOperationException("Unknown native UI command.");
        }
    }

    private static SteamAccountHistoryItem FindAccount(string steamId) => AppState.AccountHistoryService.Load().FirstOrDefault(a => a.SteamId == steamId)
        ?? throw new InvalidOperationException("Account no longer exists. Refresh History.");

    private static void SendHistory()
    {
        // Load directly: AppState.ReloadHistory intentionally swallows errors for the WinUI UI.
        var history = AppState.AccountHistoryService.Load();
        Send("history-clear");
        foreach (var a in history)
            Send("account", a.SteamId, a.AccountName, a.PersonaName, a.EyaToken, a.TokenExpiresAt?.ToLocalTime().ToString("yyyy-MM-dd HH:mm"),
                a.PremierScore?.ToString(), a.NfaOrder, NfaWarranty.Describe(a, DateTimeOffset.Now), a.AccountStatus);
    }

    private static void SendSettings()
    {
        var s = AppState.SettingsService.Load();
        Send("settings", AppState.HasNfaApiKey ? "1" : "0", s.SteamInstallPath, AppState.SettingsService.AppFolderPath, s.PersonaName);
        var pending = AppState.GetPendingPurchase();
        Send("pending", pending?.Name, pending?.StartedAt.ToLocalTime().ToString("yyyy-MM-dd HH:mm"));
        if (s.NfaReplacingSteamId is { Length: > 0 } id)
            Send("error", $"An unfinished replacement exists for {id}, order {s.NfaReplacingOrder}. Check the order at nfa.pub before requesting another.");
    }

    private static void SendLoadout()
    {
        Send("loadout-clear");
        foreach (var w in CsWeaponCatalog.All)
            Send("weapon", w.Def.ToString(), w.DisplayName, w.Category.ToString(), w.T ? "1" : "0", w.Ct ? "1" : "0", w.Starter ? "1" : "0");
        var preset = AppState.SettingsService.Load().Loadout;
        foreach (var (slot, def) in preset.T) Send("slot", "T", slot.ToString(), def.ToString());
        foreach (var (slot, def) in preset.Ct) Send("slot", "CT", slot.ToString(), def.ToString());
    }

    private static async Task PurchaseAsync(string productId, string confirmedPrice, bool recovering)
    {
        var existing = AppState.GetPendingPurchase();
        if (!recovering && existing is not null) throw new InvalidOperationException("Finish the pending purchase first. No new purchase was sent.");
        var apiKey = recovering ? existing?.ApiKey ?? Key() : Key();
        NfaPendingPurchase pending;
        if (recovering)
        {
            pending = existing ?? throw new InvalidOperationException("No pending purchase.");
            if (DateTimeOffset.Now - pending.StartedAt > TimeSpan.FromHours(1))
                throw new InvalidOperationException("Pending purchase is older than one hour. Check your nfa.pub order before buying again.");
        }
        else
        {
            var item = (await AppState.NfaClient.GetCs2StockAsync(apiKey)).FirstOrDefault(s => s.ProductId == productId)
                ?? throw new InvalidOperationException("Product no longer available. Refresh stock.");
            if (item.Available < 1) throw new InvalidOperationException("This product is out of stock.");
            if (NfaPubClient.FormatEur(item.PriceEur) != confirmedPrice) throw new InvalidOperationException("Price changed. Refresh stock and confirm the new price.");
            pending = AppState.StartPendingPurchase(item.ProductId, item.Type, item.Name, apiKey)
                ?? throw new IOException("Cannot save the purchase recovery record. No purchase was sent.");
        }
        Send("status", recovering ? "Recovering the original purchase using its saved idempotency key..." : "Buying one account...");
        try
        {
            var purchase = await AppState.NfaClient.BuyCs2Async(apiKey, pending.Type, pending.Key);
            try
            {
                AppState.AccountHistoryService.SaveNfaAccount(purchase.SteamId, purchase.Token, purchase.OrderId, pending.StartedAt,
                    recovering ? null : NfaWarranty.ReplacementsPerOrder);
            }
            catch
            {
                var rescuePath = Path.Combine(AppState.SettingsService.AppFolderPath, "recovered-accounts.txt");
                File.AppendAllText(rescuePath, $"{purchase.SteamId}----{purchase.Token}{Environment.NewLine}");
                Send("error", $"Purchase delivered but History save failed. Account saved to {rescuePath}. Use Finish pending purchase to save it again.");
                throw;
            }
            AppState.ClearPendingPurchase();
            SendHistory();
            Send("selected", purchase.SteamId);
            Send("status", $"Account delivered and saved. Charged EUR {NfaPubClient.FormatEur(purchase.ChargedEur)}.");
        }
        catch (NfaApiException ex) when (recovering ? ex.Code is "E1001" or "E1501" or "E1502" or "E1503" :
            ex.Code is "E1001" or "E1002" or "E1003" or "E1004" or "E1101" or "E1102" or "E1103" or "E1104" or
                "E1201" or "E1202" or "E1301" or "E1401" or "E1501" or "E1502" or "E1503")
        {
            AppState.ClearPendingPurchase();
            throw;
        }
        finally { SendSettings(); }
    }

    private static async Task ReplaceAsync(string steamId, string order, string reason)
    {
        var a = FindAccount(steamId);
        if (!string.IsNullOrWhiteSpace(a.NfaReplacedBy) || !string.IsNullOrWhiteSpace(a.NfaReplaceRefusal))
            throw new InvalidOperationException("This account has already been replaced or its warranty was refused.");
        if (a.NfaReplaceUnconfirmedAt is not null || AppState.SettingsService.Load().NfaReplacingSteamId is { Length: > 0 })
            throw new InvalidOperationException("A replacement is unconfirmed. Check its order at nfa.pub before retrying.");
        if (string.IsNullOrWhiteSpace(order)) throw new InvalidOperationException("Enter the account's nfa.pub order ID.");
        var apiKey = Key();
        var s = AppState.SettingsService.Load();
        s.NfaReplacingSteamId = steamId; s.NfaReplacingOrder = order; s.NfaReplacingStartedAt = DateTimeOffset.Now;
        if (!AppState.SettingsService.Save(s)) throw new IOException("Cannot save the replacement recovery record. No request was sent.");
        Send("status", "Requesting replacement. Keep this window open until the request finishes.");
        try
        {
            var r = await AppState.NfaClient.ReplaceAsync(apiKey, order.Trim(), steamId, reason.Trim());
            // Save a delivered account before clearing the in-flight record.
            try { AppState.AccountHistoryService.SaveNfaAccount(r.SteamId, r.Token, order, a.NfaOrderDeliveredAt, r.Remaining, steamId); }
            catch
            {
                var rescuePath = Path.Combine(AppState.SettingsService.AppFolderPath, "recovered-accounts.txt");
                File.AppendAllText(rescuePath, $"{r.SteamId}----{r.Token}{Environment.NewLine}");
                Send("error", $"Replacement delivered but History save failed. Account saved to {rescuePath}.");
                throw;
            }
            ClearReplacement();
            SendHistory(); Send("selected", r.SteamId);
            Send("status", $"Replacement saved. {r.Remaining} replacements left on this order.");
        }
        catch (NfaApiException ex) when (ex.Code is not ("TIMEOUT" or "NETWORK" or "UNREADABLE" or "UNCONFIRMED" or "E1000"))
        {
            if (ex.Code is "E1607" or "E1616" or "E1617" or "E1618")
                AppState.AccountHistoryService.SaveNfaReplaceOutcome(steamId, null, ex.Code);
            ClearReplacement();
            throw;
        }
        catch
        {
            AppState.AccountHistoryService.SaveNfaReplaceOutcome(steamId, s.NfaReplacingStartedAt, null);
            throw;
        }
    }

    private static void ClearReplacement()
    {
        var s = AppState.SettingsService.Load();
        s.NfaReplacingSteamId = null; s.NfaReplacingOrder = null; s.NfaReplacingStartedAt = null;
        if (!AppState.SettingsService.Save(s)) throw new IOException("Could not clear the replacement record. Check the order before retrying.");
    }
}
