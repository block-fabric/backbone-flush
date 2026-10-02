// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/graffitidialog.h>

#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <script/script.h>
#include <util/strencodings.h>

#include <QCheckBox>
#include <QDateTime>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {
using NodeRpc::Item;
using NodeRpc::Text;

/** The data an OP_RETURN script carries: what it pushes, or else everything after the opcode. */
std::vector<unsigned char> Payload(const std::vector<unsigned char>& script_bytes)
{
    const CScript script{script_bytes.begin(), script_bytes.end()};
    std::vector<unsigned char> payload;
    auto pc{script.begin()};
    opcodetype opcode;
    std::vector<unsigned char> push;
    if (!script.GetOp(pc, opcode) || opcode != OP_RETURN) return payload;
    const auto rest{pc};
    while (pc < script.end()) {
        if (!script.GetOp(pc, opcode, push)) return {rest, script.end()};
        payload.insert(payload.end(), push.begin(), push.end());
    }
    return payload;
}

/** The payload as text if it is text, and in hexadecimal otherwise. */
QString PayloadText(const std::vector<unsigned char>& payload)
{
    const bool printable{!payload.empty() && std::all_of(payload.begin(), payload.end(), [](unsigned char c) { return c >= 0x80 || c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7f); })};
    if (printable) {
        const QByteArray bytes{reinterpret_cast<const char*>(payload.data()), static_cast<qsizetype>(payload.size())};
        const QString text{QString::fromUtf8(bytes)};
        if (!text.contains(QChar::ReplacementCharacter)) return text;
    }
    return QStringLiteral("0x") + QString::fromStdString(HexStr(payload));
}
} // namespace

GraffitiDialog::GraffitiDialog(NodeRpc::WalletNameFn wallet_name, QWidget* parent)
    : QDialog(parent, GUIUtil::dialog_flags | Qt::WindowMinMaxButtonsHint), m_wallet_name{std::move(wallet_name)}
{
    setWindowTitle(tr("OP_RETURN Graffiti"));
    auto* layout{new QVBoxLayout(this)};

    auto* write{new QGroupBox(tr("Write on the chain"), this)};
    auto* write_layout{new QVBoxLayout(write)};
    auto* intro{new QLabel(tr("Publish a message in a transaction of the open wallet. It costs the transaction fee, "
                              "it is public, and it stays on the chain forever."), write)};
    intro->setWordWrap(true);
    write_layout->addWidget(intro);
    auto* row{new QHBoxLayout};
    m_message = new QLineEdit(write);
    m_message->setObjectName("graffitiMessage");
    m_message->setPlaceholderText(tr("Message"));
    auto* publish_button{new QPushButton(tr("Publish"), write)};
    publish_button->setObjectName("graffitiPublish");
    row->addWidget(m_message, 1);
    row->addWidget(publish_button);
    write_layout->addLayout(row);
    auto* options{new QHBoxLayout};
    m_hex = new QCheckBox(tr("The message is hexadecimal data"), write);
    m_size = new QLabel(write);
    options->addWidget(m_hex);
    options->addStretch();
    options->addWidget(m_size);
    write_layout->addLayout(options);
    layout->addWidget(write);

    auto* read{new QGroupBox(tr("Recent graffiti"), this)};
    auto* read_layout{new QVBoxLayout(read)};
    auto* controls{new QHBoxLayout};
    m_count = new QSpinBox(read);
    m_count->setRange(1, 5000);
    m_count->setValue(100);
    m_count->setPrefix(tr("Last "));
    m_count->setSuffix(tr(" blocks"));
    m_coinbase = new QCheckBox(tr("Include what miners wrote in coinbase transactions"), read);
    auto* refresh_button{new QPushButton(tr("Refresh"), read)};
    controls->addWidget(m_count);
    controls->addWidget(m_coinbase);
    controls->addStretch();
    controls->addWidget(refresh_button);
    read_layout->addLayout(controls);
    m_table = new QTableWidget(0, 4, read);
    m_table->setObjectName("graffitiTable");
    m_table->setHorizontalHeaderLabels({tr("Height"), tr("Time"), tr("Message"), tr("Transaction id")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(true);
    read_layout->addWidget(m_table);
    layout->addWidget(read, 1);

    connect(publish_button, &QPushButton::clicked, this, &GraffitiDialog::publish);
    connect(m_message, &QLineEdit::textChanged, this, &GraffitiDialog::updateSize);
    connect(m_hex, &QCheckBox::toggled, this, &GraffitiDialog::updateSize);
    connect(refresh_button, &QPushButton::clicked, this, &GraffitiDialog::refresh);
    connect(m_coinbase, &QCheckBox::toggled, this, &GraffitiDialog::refresh);

    updateSize();
    resize(900, 600);
    GUIUtil::handleCloseWindowShortcut(this);
}

void GraffitiDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    refresh();
}

QByteArray GraffitiDialog::message() const
{
    if (!m_hex->isChecked()) return m_message->text().toUtf8();
    const auto bytes{TryParseHex<unsigned char>(m_message->text().trimmed().toStdString())};
    if (!bytes) return {};
    return QByteArray{reinterpret_cast<const char*>(bytes->data()), static_cast<qsizetype>(bytes->size())};
}

void GraffitiDialog::updateSize()
{
    m_size->setText(tr("Size in bytes: %1").arg(message().size()));
}

void GraffitiDialog::publish()
{
    const QByteArray data{message()};
    if (data.isEmpty()) {
        QMessageBox::information(this, windowTitle(), m_hex->isChecked() ? tr("Enter hexadecimal data.") : tr("Enter a message."));
        return;
    }
    const auto wallet{m_wallet_name()};
    if (!wallet) {
        QMessageBox::information(this, windowTitle(), tr("Open or create a wallet first; publishing costs a transaction fee."));
        return;
    }
    if (QMessageBox::question(this, windowTitle(), tr("Publish this message (bytes: %1) on the chain, for everyone to see, forever?").arg(data.size())) != QMessageBox::Yes) return;

    UniValue output(UniValue::VOBJ);
    output.pushKV("data", HexStr(std::span{reinterpret_cast<const unsigned char*>(data.constData()), static_cast<size_t>(data.size())}));
    UniValue outputs(UniValue::VARR);
    outputs.push_back(output);
    UniValue params(UniValue::VARR);
    params.push_back(outputs);
    QString error;
    const auto result{NodeRpc::Call(m_client_model, "send", params, error, wallet)};
    if (!result) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    if (!result->exists("txid")) {
        QMessageBox::warning(this, windowTitle(), tr("The wallet could not sign the transaction."));
        return;
    }
    m_message->clear();
    QMessageBox::information(this, windowTitle(), tr("Published in transaction %1. It shows in the list once it is mined.").arg(Text((*result)["txid"])));
}

void GraffitiDialog::refresh()
{
    QString error;
    const auto tip{NodeRpc::Call(m_client_model, "getbestblockhash", UniValue{UniValue::VARR}, error)};
    if (!tip) return;
    m_table->setRowCount(0);
    QString hash{Text(*tip)};
    for (int i{0}; i < m_count->value() && !hash.isEmpty(); ++i) {
        UniValue params(UniValue::VARR);
        params.push_back(hash.toStdString());
        params.push_back(2);
        const auto block{NodeRpc::Call(m_client_model, "getblock", params, error)};
        if (!block) break;
        const QString height{Text((*block)["height"])};
        const QString time{QLocale().toString(QDateTime::fromSecsSinceEpoch((*block)["time"].getInt<int64_t>()), QLocale::ShortFormat)};
        const auto& txs{(*block)["tx"].getValues()};
        for (size_t t{m_coinbase->isChecked() ? 0U : 1U}; t < txs.size(); ++t) {
            for (const UniValue& out : txs[t]["vout"].getValues()) {
                const UniValue& script{out["scriptPubKey"]};
                if (Text(script["type"]) != "nulldata") continue;
                const auto payload{Payload(ParseHex(script["hex"].get_str()))};
                if (payload.empty()) continue;
                const int row{m_table->rowCount()};
                m_table->insertRow(row);
                m_table->setItem(row, 0, Item(height));
                m_table->setItem(row, 1, Item(time));
                m_table->setItem(row, 2, Item(PayloadText(payload)));
                m_table->setItem(row, 3, Item(Text(txs[t]["txid"])));
            }
        }
        hash = block->exists("previousblockhash") ? Text((*block)["previousblockhash"]) : QString{};
    }
    m_table->resizeColumnsToContents();
    m_table->setColumnWidth(2, std::min(m_table->columnWidth(2), 480));
}
