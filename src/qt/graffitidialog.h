// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_GRAFFITIDIALOG_H
#define BITCOIN_QT_GRAFFITIDIALOG_H

#include <qt/noderpc.h>

#include <QDialog>

class ClientModel;

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;
QT_END_NAMESPACE

/** Window to read the data that transactions carry in OP_RETURN outputs, and to publish some. */
class GraffitiDialog : public QDialog
{
    Q_OBJECT

public:
    explicit GraffitiDialog(NodeRpc::WalletNameFn wallet_name, QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model) { m_client_model = client_model; }

public Q_SLOTS:
    void refresh();

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void publish();
    void updateSize();

private:
    QByteArray message() const;

    ClientModel* m_client_model{nullptr};
    NodeRpc::WalletNameFn m_wallet_name;
    QLineEdit* m_message;
    QCheckBox* m_hex;
    QLabel* m_size;
    QSpinBox* m_count;
    QCheckBox* m_coinbase;
    QTableWidget* m_table;
};

#endif // BITCOIN_QT_GRAFFITIDIALOG_H
